#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

/* 进程寿命模型：runtime 只初始化一次，重复 Start 抛 std::logic_error。
 * 每个测试文件就是一个可执行，这里把用例内的 Start() 收敛为进程内一次初始化。 */
namespace
{
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, [](){
        bbt::coroutine::detail::Scheduler::GetInstance()->Start();
    });
}
}

using namespace bbt::coroutine::detail;

namespace
{

bool WaitForAtLeast(const std::atomic_int& value, int expected,
                    std::chrono::steady_clock::duration timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (value.load(std::memory_order_acquire) < expected)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

class StaticProcesserCountRestore
{
public:
    explicit StaticProcesserCountRestore(GlobalConfig& config):
        m_config(config),
        m_previous_count(config.m_cfg_static_thread_num)
    {
    }

    ~StaticProcesserCountRestore()
    {
        Restore();
    }

    void Restore()
    {
        m_config.m_cfg_static_thread_num = m_previous_count;
    }

    size_t PreviousCount() const
    {
        return m_previous_count;
    }

private:
    GlobalConfig& m_config;
    const size_t m_previous_count;
};

class SchedulerStopGuard
{
public:
    explicit SchedulerStopGuard(Scheduler& scheduler):
        m_scheduler(scheduler)
    {
    }

    ~SchedulerStopGuard()
    {
        Stop();
    }

    void MarkStarted()
    {
        m_started = true;
    }

    void Stop()
    {
        /* 进程寿命：运行时不再有停机入口（Scheduler::Stop 已删除）。
         * 本 guard 保留为无操作——用例级停机不存在，也不新增替代停机路径。 */
        if (!m_started)
            return;
        m_started = false;
    }

private:
    Scheduler& m_scheduler;
    bool m_started{false};
};

class ExceptionCallbackRestore
{
public:
    explicit ExceptionCallbackRestore(GlobalConfig& config):
        m_config(config),
        m_previous(config.m_ext_coevent_exception_callback)
    {
    }

    ~ExceptionCallbackRestore()
    {
        m_config.m_ext_coevent_exception_callback = std::move(m_previous);
    }

private:
    GlobalConfig& m_config;
    std::function<void(const bbt::core::errcode::IErrcode&)> m_previous;
};

}

BOOST_AUTO_TEST_SUITE(CoPollEventStateTest)

BOOST_AUTO_TEST_CASE(t_custom_trigger_before_regist_is_pending_until_commit)
{
    std::atomic_int callback_count{0};
    std::atomic_int callback_flags{POLL_EVENT_DEFAULT};
    std::atomic_int callback_key{-1};
    auto event = CoPollEvent::Create(1, [&](auto, int flags, int key) {
        callback_count.fetch_add(1);
        callback_flags.store(flags);
        callback_key.store(key);
    });

    BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
    BOOST_REQUIRE_EQUAL(event->Trigger(POLL_EVENT_CUSTOM), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_CHECK_EQUAL(callback_count.load(), 0);

    BOOST_REQUIRE(event->CommitPark());
    BOOST_CHECK_EQUAL(callback_count.load(), 1);
    BOOST_CHECK_EQUAL(callback_flags.load(), POLL_EVENT_CUSTOM);
    BOOST_CHECK_EQUAL(callback_key.load(), POLL_EVENT_CUSTOM_COND);
    BOOST_CHECK_EQUAL(event->CommitPark(), false);
    BOOST_CHECK_EQUAL(callback_count.load(), 1);
}

BOOST_AUTO_TEST_CASE(t_pending_callback_exception_does_not_escape_commit_park)
{
    std::atomic_int callback_count{0};
    auto event = CoPollEvent::Create(1, [&](auto, int, int) {
        callback_count.fetch_add(1);
        throw std::runtime_error("callback failure");
    });

    BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
    BOOST_REQUIRE_EQUAL(event->Trigger(POLL_EVENT_CUSTOM), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);

    bool completed_pending_event = false;
    BOOST_CHECK_NO_THROW(completed_pending_event = event->CommitPark());
    BOOST_CHECK(completed_pending_event);
    BOOST_CHECK_EQUAL(callback_count.load(), 1);
    BOOST_CHECK(event->IsFinal());
    BOOST_CHECK_EQUAL(event->GetStatus(), POLLEVENT_FINAL);
}

BOOST_AUTO_TEST_CASE(t_parked_callback_exception_does_not_escape_trigger)
{
    ExceptionCallbackRestore callback_restore(*g_bbt_coroutine_config);
    g_bbt_coroutine_config->m_ext_coevent_exception_callback = nullptr;
    const auto before = g_bbt_coroutine_config->m_unhandled_exception_count.load();
    std::atomic_int callback_count{0};
    auto event = CoPollEvent::Create(1, [&](auto, int, int) {
        callback_count.fetch_add(1);
        throw std::runtime_error("parked callback failure");
    });

    BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_CHECK_EQUAL(event->CommitPark(), false);

    int trigger_result = -1;
    BOOST_CHECK_NO_THROW(trigger_result = event->Trigger(POLL_EVENT_CUSTOM));
    BOOST_CHECK_EQUAL(trigger_result, 0);
    BOOST_CHECK_EQUAL(callback_count.load(), 1);
    BOOST_CHECK(event->IsFinal());
    BOOST_CHECK_EQUAL(event->GetStatus(), POLLEVENT_FINAL);
    BOOST_CHECK_EQUAL(g_bbt_coroutine_config->m_unhandled_exception_count.load(), before + 1);
}

BOOST_AUTO_TEST_CASE(t_regist_commit_park_then_trigger_completes_once)
{
    std::atomic_int callback_count{0};
    std::atomic_int callback_flags{POLL_EVENT_DEFAULT};
    auto event = CoPollEvent::Create(1, [&](auto, int flags, int) {
        callback_count.fetch_add(1);
        callback_flags.store(flags);
    });

    BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_CHECK_EQUAL(callback_count.load(), 0);
    BOOST_CHECK_EQUAL(event->CommitPark(), false);
    BOOST_CHECK_EQUAL(callback_count.load(), 0);

    BOOST_REQUIRE_EQUAL(event->Trigger(POLL_EVENT_CUSTOM), 0);
    BOOST_CHECK_EQUAL(callback_count.load(), 1);
    BOOST_CHECK_EQUAL(callback_flags.load(), POLL_EVENT_CUSTOM);
    BOOST_CHECK(event->IsFinal());
}

BOOST_AUTO_TEST_CASE(t_repeated_trigger_has_one_winner)
{
    std::atomic_int callback_count{0};
    std::atomic_int callback_flags{POLL_EVENT_DEFAULT};
    auto event = CoPollEvent::Create(1, [&](auto, int flags, int) {
        callback_count.fetch_add(1);
        callback_flags.store(flags);
    });

    BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_REQUIRE_EQUAL(event->CommitPark(), false);

    BOOST_REQUIRE_EQUAL(event->Trigger(POLL_EVENT_CUSTOM), 0);
    BOOST_CHECK_EQUAL(event->Trigger(POLL_EVENT_TIMEOUT), -1);
    BOOST_CHECK_EQUAL(callback_count.load(), 1);
    BOOST_CHECK_EQUAL(callback_flags.load(), POLL_EVENT_CUSTOM);
}

BOOST_AUTO_TEST_CASE(t_trigger_and_unregister_compete_for_parked_event)
{
    std::atomic_int callback_count{0};
    auto event = CoPollEvent::Create(1, [&](auto, int, int) {
        callback_count.fetch_add(1);
    });

    BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_REQUIRE_EQUAL(event->CommitPark(), false);

    std::atomic_int ready_count{0};
    std::atomic_bool start{false};
    std::atomic_int trigger_result{std::numeric_limits<int>::min()};
    std::atomic_int unregister_result{std::numeric_limits<int>::min()};
    std::thread trigger_thread([&] {
        ready_count.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        trigger_result.store(event->Trigger(POLL_EVENT_CUSTOM));
    });
    std::thread unregister_thread([&] {
        ready_count.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        unregister_result.store(event->UnRegist());
    });

    const bool ready_result = WaitForAtLeast(ready_count, 2, std::chrono::seconds(1));
    start.store(true, std::memory_order_release);
    trigger_thread.join();
    unregister_thread.join();

    BOOST_CHECK(ready_result);
    BOOST_CHECK_EQUAL(trigger_result.load() == 0, unregister_result.load() != 0);
    BOOST_CHECK_EQUAL(callback_count.load(), trigger_result.load() == 0 ? 1 : 0);
    BOOST_CHECK(event->IsFinal());
}

BOOST_AUTO_TEST_CASE(t_pending_trigger_flags_preserve_first_outcome)
{
    std::atomic_int callback_count{0};
    std::atomic_int callback_flags{POLL_EVENT_DEFAULT};
    auto event = CoPollEvent::Create(1, [&](auto, int flags, int) {
        callback_count.fetch_add(1);
        callback_flags.store(flags);
    });
    const short first_flags = static_cast<short>(POLL_EVENT_CUSTOM | POLL_EVENT_TIMEOUT);

    BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
    BOOST_REQUIRE_EQUAL(event->Trigger(first_flags), 0);
    BOOST_CHECK_EQUAL(event->Trigger(POLL_EVENT_READABLE), -1);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_REQUIRE(event->CommitPark());

    BOOST_CHECK_EQUAL(callback_count.load(), 1);
    BOOST_CHECK_EQUAL(callback_flags.load(), first_flags);
}

BOOST_AUTO_TEST_CASE(t_final_event_cannot_be_reused)
{
    std::atomic_int callback_count{0};
    auto event = CoPollEvent::Create(1, [&](auto, int, int) {
        callback_count.fetch_add(1);
    });

    BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_REQUIRE_EQUAL(event->CommitPark(), false);
    BOOST_REQUIRE_EQUAL(event->Trigger(POLL_EVENT_CUSTOM), 0);
    BOOST_REQUIRE(event->IsFinal());

    BOOST_CHECK_EQUAL(callback_count.load(), 1);
    BOOST_CHECK_EQUAL(event->Trigger(POLL_EVENT_CUSTOM), -1);
    BOOST_CHECK_EQUAL(event->Regist(), -1);
    BOOST_CHECK_EQUAL(event->UnRegist(), -1);
    BOOST_CHECK_EQUAL(event->CommitPark(), false);
    BOOST_CHECK_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_CHAN, nullptr), -1);
}

BOOST_AUTO_TEST_CASE(t_cancelled_event_cannot_be_reused)
{
    std::atomic_int callback_count{0};
    auto event = CoPollEvent::Create(1, [&](auto, int, int) {
        callback_count.fetch_add(1);
    });

    BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_REQUIRE_EQUAL(event->CommitPark(), false);
    BOOST_REQUIRE_EQUAL(event->UnRegist(), 0);
    BOOST_REQUIRE(event->IsFinal());

    BOOST_CHECK_EQUAL(callback_count.load(), 0);
    BOOST_CHECK_EQUAL(event->Trigger(POLL_EVENT_CUSTOM), -1);
    BOOST_CHECK_EQUAL(event->Regist(), -1);
    BOOST_CHECK_EQUAL(event->UnRegist(), -1);
    BOOST_CHECK_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_CHAN, nullptr), -1);
    BOOST_CHECK_EQUAL(callback_count.load(), 0);
}

BOOST_AUTO_TEST_CASE(t_trigger_and_commit_park_race_completes_each_event_once)
{
    constexpr int kRounds = 256;

    for (int round = 0; round < kRounds; ++round)
    {
        std::atomic_int callback_count{0};
        auto event = CoPollEvent::Create(round + 1, [&](auto, int, int) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        });

        BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
        BOOST_REQUIRE_EQUAL(event->Regist(), 0);

        std::atomic_bool start{false};
        std::atomic_int ready_count{0};
        std::atomic_int trigger_result{std::numeric_limits<int>::min()};
        std::thread trigger_thread([&] {
            ready_count.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            trigger_result.store(event->Trigger(POLL_EVENT_CUSTOM), std::memory_order_release);
        });
        std::thread commit_thread([&] {
            ready_count.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            event->CommitPark();
        });

        const bool ready_result = WaitForAtLeast(ready_count, 2, std::chrono::seconds(1));
        start.store(true, std::memory_order_release);
        trigger_thread.join();
        commit_thread.join();

        BOOST_CHECK(ready_result);
        BOOST_CHECK_EQUAL(trigger_result.load(std::memory_order_acquire), 0);
        BOOST_CHECK_EQUAL(callback_count.load(std::memory_order_acquire), 1);
        BOOST_CHECK(event->IsFinal());
        BOOST_CHECK_EQUAL(event->GetStatus(), POLLEVENT_FINAL);
    }
}

BOOST_AUTO_TEST_CASE(t_multiple_trigger_threads_have_one_winner)
{
    constexpr int kRounds = 64;
    constexpr int kTriggerThreads = 8;
    for (int round = 0; round < kRounds; ++round)
    {
        std::atomic_int callback_count{0};
        auto event = CoPollEvent::Create(round + 1, [&](auto, int, int) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        });

        BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
        BOOST_REQUIRE_EQUAL(event->Regist(), 0);
        BOOST_REQUIRE_EQUAL(event->CommitPark(), false);

        std::atomic_bool start{false};
        std::atomic_int ready_count{0};
        std::array<int, kTriggerThreads> trigger_results{};
        std::vector<std::thread> trigger_threads;
        trigger_threads.reserve(kTriggerThreads);
        for (int i = 0; i < kTriggerThreads; ++i)
        {
            trigger_threads.emplace_back([&, i] {
                ready_count.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                trigger_results[i] = event->Trigger(POLL_EVENT_CUSTOM);
            });
        }

        const bool ready_result = WaitForAtLeast(ready_count, kTriggerThreads,
                                                 std::chrono::seconds(1));
        start.store(true, std::memory_order_release);
        for (auto& thread : trigger_threads)
            thread.join();

        int winners = 0;
        for (const int result : trigger_results)
            winners += result == 0 ? 1 : 0;

        BOOST_CHECK(ready_result);
        BOOST_CHECK_EQUAL(winners, 1);
        BOOST_CHECK_EQUAL(callback_count.load(std::memory_order_acquire), 1);
        BOOST_CHECK(event->IsFinal());
        BOOST_CHECK_EQUAL(event->GetStatus(), POLLEVENT_FINAL);
    }
}

BOOST_AUTO_TEST_CASE(t_timeout_and_custom_trigger_have_one_first_outcome)
{
    constexpr int kTimeoutMs = 5;

    {
        std::atomic_int callback_count{0};
        std::atomic_int callback_flags{POLL_EVENT_DEFAULT};
        auto event = CoPollEvent::Create(1, [&](auto, int flags, int) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
            callback_flags.store(flags, std::memory_order_release);
        });

        BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
        BOOST_REQUIRE_EQUAL(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, kTimeoutMs), 0);
        BOOST_REQUIRE_EQUAL(event->Regist(), 0);
        BOOST_REQUIRE_EQUAL(event->Trigger(POLL_EVENT_CUSTOM), 0);
        BOOST_REQUIRE(event->CommitPark());

        BOOST_CHECK_EQUAL(callback_count.load(std::memory_order_acquire), 1);
        BOOST_CHECK_EQUAL(callback_flags.load(std::memory_order_acquire), POLL_EVENT_CUSTOM);
        BOOST_CHECK(event->IsFinal());
        BOOST_CHECK_EQUAL(event->GetStatus(), POLLEVENT_FINAL);
        BOOST_CHECK_EQUAL(event->Trigger(POLL_EVENT_TIMEOUT), -1);
        g_bbt_poller->PollOnce();
        BOOST_CHECK_EQUAL(callback_count.load(std::memory_order_acquire), 1);
    }

    {
        std::atomic_int callback_count{0};
        std::atomic_int callback_flags{POLL_EVENT_DEFAULT};
        auto event = CoPollEvent::Create(3, [&](auto, int flags, int) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
            callback_flags.store(flags, std::memory_order_release);
        });

        BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
        BOOST_REQUIRE_EQUAL(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, 5), 0);
        BOOST_REQUIRE_EQUAL(event->Regist(), 0);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (event->GetStatus() != POLLEVENT_TRIGGER &&
               std::chrono::steady_clock::now() < deadline)
        {
            g_bbt_poller->PollOnce();
            if (event->GetStatus() != POLLEVENT_TRIGGER)
                std::this_thread::yield();
        }

        BOOST_REQUIRE_EQUAL(callback_count.load(std::memory_order_acquire), 0);
        // 内部 PENDING 阶段对外映射为 POLLEVENT_TRIGGER。
        BOOST_REQUIRE_EQUAL(event->GetStatus(), POLLEVENT_TRIGGER);

        BOOST_REQUIRE(event->CommitPark());
        BOOST_CHECK_EQUAL(callback_count.load(std::memory_order_acquire), 1);
        BOOST_CHECK_EQUAL(callback_flags.load(std::memory_order_acquire), POLL_EVENT_TIMEOUT);
        BOOST_CHECK(event->IsFinal());
        BOOST_CHECK_EQUAL(event->GetStatus(), POLLEVENT_FINAL);
    }

    {
        std::atomic_int callback_count{0};
        std::atomic_int callback_flags{POLL_EVENT_DEFAULT};
        auto event = CoPollEvent::Create(2, [&](auto, int flags, int) {
            callback_count.fetch_add(1, std::memory_order_relaxed);
            callback_flags.store(flags, std::memory_order_release);
        });

        BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
        BOOST_REQUIRE_EQUAL(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, kTimeoutMs), 0);
        BOOST_REQUIRE_EQUAL(event->Regist(), 0);
        BOOST_REQUIRE_EQUAL(event->CommitPark(), false);

        bool poll_dispatched = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (callback_count.load(std::memory_order_acquire) == 0 &&
               std::chrono::steady_clock::now() < deadline)
        {
            poll_dispatched = g_bbt_poller->PollOnce() || poll_dispatched;
            if (callback_count.load(std::memory_order_acquire) == 0)
                std::this_thread::yield();
        }

        BOOST_REQUIRE_EQUAL(callback_count.load(std::memory_order_acquire), 1);
        BOOST_CHECK(poll_dispatched);
        BOOST_CHECK_EQUAL(callback_flags.load(std::memory_order_acquire), POLL_EVENT_TIMEOUT);
        BOOST_CHECK(event->IsFinal());
        BOOST_CHECK_EQUAL(event->GetStatus(), POLLEVENT_FINAL);
        BOOST_CHECK_EQUAL(event->Trigger(POLL_EVENT_CUSTOM), -1);
        g_bbt_poller->PollOnce();
        BOOST_CHECK_EQUAL(callback_count.load(std::memory_order_acquire), 1);
    }
}

BOOST_AUTO_TEST_CASE(t_system_readable_event_can_be_pending_before_commit)
{
    int fds[2];
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);

    std::atomic_int callback_count{0};
    std::atomic_int callback_flags{POLL_EVENT_DEFAULT};
    auto event = CoPollEvent::Create(1, [&](auto, int flags, int) {
        callback_count.fetch_add(1, std::memory_order_relaxed);
        callback_flags.store(flags, std::memory_order_release);
    });

    BOOST_REQUIRE_EQUAL(event->InitFdEvent(fds[0], bbt::pollevent::EventOpt::READABLE, 0), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);

    const char signal = 'x';
    BOOST_REQUIRE_EQUAL(::write(fds[1], &signal, sizeof(signal)), static_cast<ssize_t>(sizeof(signal)));
    BOOST_REQUIRE(g_bbt_poller->PollOnce());
    BOOST_CHECK_EQUAL(callback_count.load(std::memory_order_acquire), 0);

    BOOST_REQUIRE(event->CommitPark());
    BOOST_CHECK_EQUAL(callback_count.load(std::memory_order_acquire), 1);
    BOOST_CHECK_EQUAL(callback_flags.load(std::memory_order_acquire),
                      bbt::pollevent::EventOpt::READABLE);
    BOOST_CHECK(event->IsFinal());
    BOOST_CHECK_EQUAL(event->GetStatus(), POLLEVENT_FINAL);

    g_bbt_poller->PollOnce();
    BOOST_CHECK_EQUAL(callback_count.load(std::memory_order_acquire), 1);
    BOOST_CHECK_EQUAL(::close(fds[0]), 0);
    BOOST_CHECK_EQUAL(::close(fds[1]), 0);
}

BOOST_AUTO_TEST_CASE(t_system_readable_event_completes_once_after_commit)
{
    int fds[2];
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);

    std::atomic_int callback_count{0};
    auto event = CoPollEvent::Create(1, [&](auto, int flags, int) {
        BOOST_CHECK_EQUAL(flags, bbt::pollevent::EventOpt::READABLE);
        callback_count.fetch_add(1, std::memory_order_relaxed);
    });

    BOOST_REQUIRE_EQUAL(event->InitFdEvent(fds[0], bbt::pollevent::EventOpt::READABLE, 0), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_REQUIRE_EQUAL(event->CommitPark(), false);

    const char signal = 'x';
    BOOST_REQUIRE_EQUAL(::write(fds[1], &signal, sizeof(signal)), static_cast<ssize_t>(sizeof(signal)));
    BOOST_REQUIRE(g_bbt_poller->PollOnce());
    BOOST_CHECK_EQUAL(callback_count.load(std::memory_order_acquire), 1);
    BOOST_CHECK(event->IsFinal());
    BOOST_CHECK_EQUAL(event->GetStatus(), POLLEVENT_FINAL);

    g_bbt_poller->PollOnce();
    BOOST_CHECK_EQUAL(callback_count.load(std::memory_order_acquire), 1);
    BOOST_CHECK_EQUAL(::close(fds[0]), 0);
    BOOST_CHECK_EQUAL(::close(fds[1]), 0);
}

BOOST_AUTO_TEST_CASE(t_peer_close_completes_readable_wait)
{
    int fds[2];
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);

    std::atomic_int n{0};
    auto event = CoPollEvent::Create(1, [&](auto, int, int){ n.fetch_add(1); });

    BOOST_REQUIRE_EQUAL(event->InitFdEvent(fds[0], bbt::pollevent::EventOpt::READABLE, 0), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_REQUIRE_EQUAL(event->CommitPark(), false);

    BOOST_REQUIRE_EQUAL(::close(fds[1]), 0);
    fds[1] = -1;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (n.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        CoPoller::GetInstance()->PollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    BOOST_CHECK_EQUAL(n.load(), 1);
    BOOST_CHECK(event->IsFinal());
    BOOST_CHECK_EQUAL(event->Trigger(POLL_EVENT_CUSTOM), -1);

    event->UnRegist();
    BOOST_CHECK_EQUAL(::close(fds[0]), 0);
    fds[0] = -1;
}

BOOST_AUTO_TEST_CASE(t_destroy_parked_event_does_not_callback)
{
    std::atomic_int n{0};
    {
        auto event = CoPollEvent::Create(1, [&](auto, int, int){ n.fetch_add(1); });
        BOOST_REQUIRE_EQUAL(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, 40), 0);
        BOOST_REQUIRE_EQUAL(event->Regist(), 0);
        BOOST_REQUIRE_EQUAL(event->CommitPark(), false);
        event.reset();
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline) {
        CoPoller::GetInstance()->PollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    BOOST_CHECK_EQUAL(n.load(), 0);
}

BOOST_AUTO_TEST_CASE(t_multi_processer_yield_requeues_each_coroutine_once_per_iteration)
{
    constexpr int kProcesserCount = 3;
    constexpr int kCoroutineCount = 12;
    constexpr int kIterations = 32;
    std::array<std::atomic_int, kCoroutineCount> entered;
    std::array<std::atomic_int, kCoroutineCount> exited;
    std::array<std::atomic_int, kCoroutineCount> active;
    std::array<std::atomic_int, kCoroutineCount> completed_per_coroutine;
    for (int i = 0; i < kCoroutineCount; ++i)
    {
        entered[i].store(0, std::memory_order_relaxed);
        exited[i].store(0, std::memory_order_relaxed);
        active[i].store(0, std::memory_order_relaxed);
        completed_per_coroutine[i].store(0, std::memory_order_relaxed);
    }

    std::atomic_int duplicate_resume{0};
    std::atomic_int registration_failures{0};
    std::atomic_int completed_count{0};
    std::mutex completed_mutex;
    std::condition_variable completed_cond;
    auto& config = *g_bbt_coroutine_config;
    StaticProcesserCountRestore restore_processer_count{config};
    config.m_cfg_static_thread_num = kProcesserCount;
    auto& scheduler = g_scheduler;
    SchedulerStopGuard scheduler_stop{*scheduler};
    EnsureRuntime();
    scheduler_stop.MarkStarted();

    for (int coroutine_index = 0; coroutine_index < kCoroutineCount; ++coroutine_index)
    {
        bool registered = false;
        scheduler->RegistCoroutineTask([&, coroutine_index] {
            for (int iteration = 0; iteration < kIterations; ++iteration)
            {
                if (active[coroutine_index].fetch_add(1, std::memory_order_acq_rel) != 0)
                    duplicate_resume.fetch_add(1, std::memory_order_relaxed);
                entered[coroutine_index].fetch_add(1, std::memory_order_relaxed);

                if (iteration + 1 < kIterations)
                    bbtco_yield;

                active[coroutine_index].fetch_sub(1, std::memory_order_release);
                exited[coroutine_index].fetch_add(1, std::memory_order_relaxed);
            }
            completed_per_coroutine[coroutine_index].fetch_add(1, std::memory_order_relaxed);
            if (completed_count.fetch_add(1, std::memory_order_release) + 1 == kCoroutineCount)
                completed_cond.notify_one();
        }, registered);
        if (!registered)
            registration_failures.fetch_add(1, std::memory_order_relaxed);
    }

    std::unique_lock<std::mutex> completed_lock(completed_mutex);
    const bool completed_result = completed_cond.wait_for(
        completed_lock, std::chrono::seconds(5), [&] {
            return completed_count.load(std::memory_order_acquire) == kCoroutineCount;
        });
    completed_lock.unlock();

    scheduler_stop.Stop();
    restore_processer_count.Restore();
    BOOST_REQUIRE(completed_result);
    BOOST_CHECK_EQUAL(registration_failures.load(std::memory_order_acquire), 0);
    BOOST_CHECK_EQUAL(config.m_cfg_static_thread_num, restore_processer_count.PreviousCount());
    BOOST_CHECK_EQUAL(duplicate_resume.load(std::memory_order_acquire), 0);
    for (int i = 0; i < kCoroutineCount; ++i)
    {
        BOOST_CHECK_EQUAL(entered[i].load(std::memory_order_acquire), kIterations);
        BOOST_CHECK_EQUAL(exited[i].load(std::memory_order_acquire), kIterations);
        BOOST_CHECK_EQUAL(active[i].load(std::memory_order_acquire), 0);
        BOOST_CHECK_EQUAL(completed_per_coroutine[i].load(std::memory_order_acquire), 1);
    }
}

BOOST_AUTO_TEST_CASE(t_init_fd_event_with_live_duplicate_never_throws)
{
    /* Event 为每个用户 fd 保留私有 dup，重复监听不再因
     * epoll 使用同一 fd 号而报 EEXIST；两个监听都应能注册且不抛异常。 */
    int fds[2];
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);

    auto first = CoPollEvent::Create(1, [&](auto, int, int) {});
    BOOST_REQUIRE_EQUAL(first->InitFdEvent(fds[0], bbt::pollevent::EventOpt::READABLE, 0), 0);
    BOOST_REQUIRE_EQUAL(first->Regist(), 0);

    auto second = CoPollEvent::Create(2, [&](auto, int, int) {});
    BOOST_REQUIRE_EQUAL(second->InitFdEvent(fds[0], bbt::pollevent::EventOpt::READABLE, 0), 0);
    int ret = -99;
    BOOST_CHECK_NO_THROW(ret = second->Regist());
    BOOST_CHECK_EQUAL(ret, 0);

    BOOST_CHECK_EQUAL(second->UnRegist(), 0);
    first->UnRegist();
    BOOST_CHECK_EQUAL(::close(fds[0]), 0);
    BOOST_CHECK_EQUAL(::close(fds[1]), 0);
}

BOOST_AUTO_TEST_CASE(t_init_during_close_window_is_closed_not_admitted)
{
    /* 协议级确定性复现「BeginFdClose → InitFdEvent → 底层 close →
     * EndFdClose → Regist」交错。修复前 Init 在 closing 窗口内只采到已
     * 推进的新 epoch，EndFdClose 解 closing 后 Regist 会把旧等待误当
     * 新代准入——旧等待就此混进新代 waiters 表，本应随 close 收敛
     * EBADF 的操作被当作有效等待。注意失效形态不是「旧对象物理消失
     * 就不再被唤醒」：底层 Event 构造期的私有 dup 使旧 open file
     * description 在 close 后仍存活，旧 peer 就绪仍会唤醒该事件（若
     * dup 落在 close+reuse 之后，则直接绑到同号新对象上）；两种形态
     * 都在同一 epoch 下被判有效。
     *
     * 本用例是内部协议级证据：直接驱动 CoPollEvent::BeginFdClose/EndFdClose
     * 并用单次 raw syscall 扮演 Hook_Close 内层真正的 close（不经公开
     * Hook_Close 全链路），只证明 CoPollEvent 的 epoch/closing 协议在
     * establish 窗口内不再准入旧等待；不推导对绕过 Hook 的 syscall 的支持，
     * 也不声称覆盖了 Hook_Close 端到端路径。
     *
     * 对象模型：close 目标 fds[0] 属旧对象（旧 peer fds[1] 全程存活，旧
     * 对象在 close 前确实可读写）；dup2 的复用源 np[0] 来自独立
     * socketpair，是真实独立 open file description——不是 dup(fds[0])
     * 那种与旧对象共享 file description 的别名。
     *
     * 有界门固定时序（单线程 joinable 期间不做可能展开的致命断言；线程
     * 入口只存错误与阶段，断言全部在主线程 join 之后）：
     *   主线程 : BeginFdClose → 放行 → 等 Init 完成 → raw close →
     *            dup2(np[0]→fds[0]) → EndFdClose → 放行 → 等完成收据
     *   T_init : InitFdEvent（closing 窗口内采样）→ 等放行 → Regist
     * 任一环节超时：交由 stop_on_timeout lambda 中止测试进程（T_init 是
     * joinable std::thread、仍借用本用例栈，超时在借用释放之前 abort——
     * 进程死在栈展开前，不 UAF 继续下一用例）。
     */

    int fds[2] = {-1, -1};   // close 目标：旧对象 [0] + 存活的旧 peer [1]
    int np[2]  = {-1, -1};   // 独立复用源：dup2(np[0]→fds[0])，[1] 是新 peer
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, np), 0);

    /* 新对象身份/数据判别预置：np[0] 自带 O_NONBLOCK，np[1] 预放 'N'。
     * 旧 peer 侧 fds[1] 预放 'O'——close 后旧对象对新号不再可达，若
     * 协议失效让旧等待被当新代准入，后续可观察到的是 np 的对象。 */
    const int fl_new_src = ::fcntl(np[0], F_GETFL, 0);
    BOOST_REQUIRE(fl_new_src >= 0);
    BOOST_REQUIRE_EQUAL(::fcntl(np[0], F_SETFL, fl_new_src | O_NONBLOCK), 0);
    const char new_byte = 'N';
    const char old_byte = 'O';
    BOOST_REQUIRE_EQUAL(::write(np[1], &new_byte, 1), 1);
    BOOST_REQUIRE_EQUAL(::write(fds[1], &old_byte, 1), 1);

    std::atomic_int callback_count{0};
    std::atomic_int callback_flags{POLL_EVENT_DEFAULT};
    auto event = CoPollEvent::Create(1, [&](auto, int flags, int) {
        callback_count.fetch_add(1, std::memory_order_relaxed);
        callback_flags.store(flags, std::memory_order_release);
    });

    std::mutex gate_mu;
    std::condition_variable gate_cv;
    int gate = 0;  // 0=等 Begin 完成; 1=可做 Init; 2=Init 已回; 3=可做 Regist; 4=done
    auto set_gate = [&](int v) {
        { std::lock_guard<std::mutex> lk(gate_mu); gate = v; }
        gate_cv.notify_all();
    };
    auto wait_gate = [&](int v) -> bool {
        std::unique_lock<std::mutex> lk(gate_mu);
        return gate_cv.wait_for(lk, std::chrono::seconds(5), [&] { return gate >= v; });
    };

    /* 门超时语义：T_init 可能仍阻塞在 wait_gate 内、借用本用例栈上的
     * gate/event。此时展开栈继续跑后续用例会 UAF，所以 gate_ready
     * 为 false 时直接 BOOST_ERROR+abort——进程死在借用释放之前，
     * 而不是假装「已清理」。std::thread 析构 terminate 同理不算清理。 */
    std::atomic_bool gate_broken{false};
    auto stop_on_timeout = [&](const char* what) -> void {
        gate_broken.store(true, std::memory_order_release);
        { std::lock_guard<std::mutex> lk(gate_mu); gate = 99; }
        gate_cv.notify_all();   // 放行仍在门内的 T_init，让它尽快自然退出
        BOOST_ERROR("close-window handshake timed out at " << what
                    << "; aborting before borrowed stack unwinds");
        std::abort();
    };

    std::atomic_int init_ret{-99};
    std::atomic_int regist_ret{-99};
    std::atomic_int worker_phase{0};   // T_init 推进到的阶段（排障用）
    std::thread t_init([&] {
        if (!wait_gate(1)) { init_ret.store(-2); return; }
        /* 在 主线程已 claim closing、底层 close 未返回时建立等待——
         * 这正是被复用的同号新对象上看不见旧代的窗口。 */
        init_ret.store(event->InitFdEvent(fds[0], bbt::pollevent::EventOpt::READABLE, 0));
        worker_phase.store(1, std::memory_order_release);
        set_gate(2);
        if (!wait_gate(3)) { regist_ret.store(-2); return; }
        regist_ret.store(event->Regist());
        worker_phase.store(2, std::memory_order_release);
        set_gate(4);
    });

    /* 线程与门的 RAII 兜底：任何早退（含下面哪一步意外失败）都先放行
     * 并 join，不把仍 joinable 的 std::thread 留给析构 terminate。 */
    struct ThreadJoinGuard {
        std::thread& t;
        std::function<void(int)> release;
        ~ThreadJoinGuard() {
            if (!t.joinable()) return;
            release(99);
            t.join();
        }
    } join_guard{t_init, set_gate};

    const bool begin_ok = CoPollEvent::BeginFdClose(fds[0]);  // claim + 推进 epoch + closing
    set_gate(1);
    bool gate_ready = begin_ok && wait_gate(2);               // Init 已在 closing 窗口内完成采样
    if (!gate_ready)
        stop_on_timeout("BeginFdClose/InitFdEvent");

    /* Hook_Close 内层真正的 close（单次 raw syscall，不经公开 Hook_Close） */
    if (::syscall(SYS_close, fds[0]) != 0)
        stop_on_timeout("raw close");

    /* 立即在同号上放一个独立新对象——窗口内该 fd 数字已物理换代，
     * close 之后这个数字不再指向旧对象。注意旧端点并未因此「物理死
     * 亡」：底层 Event 若已用私有 dup 持有旧 open file description（旧
     * peer fds[1] 也仍活着），它照样可能就绪；但那不使本次等待对新代
     * 有效。 */
    if (::dup2(np[0], fds[0]) != fds[0])
        stop_on_timeout("dup2(np[0]->fds[0])");

    CoPollEvent::EndFdClose(fds[0]);                     // 解 closing；新对象属新一代
    set_gate(3);
    if (!wait_gate(4))
        stop_on_timeout("Regist");

    t_init.join();
    join_guard.release = [](int){};                      // 已 join，守卫不再动作

    /* 行为断言（修复后）：Init 成功建立等待（0），但 Regist 不得把建立于
     * 旧代的等待准入为新代有效等待——事件应被锁存为 CLOSED 首胜，由
     * CommitPark 消费 PENDING 后唯一完成，且绝不进入 waiters 表。 */
    BOOST_CHECK_EQUAL(worker_phase.load(std::memory_order_acquire), 2);
    BOOST_CHECK_EQUAL(init_ret.load(), 0);
    BOOST_CHECK_EQUAL(regist_ret.load(), 0);   // Regist 语义为「建立完成」，非「等待有效」
    BOOST_CHECK_EQUAL(CoPollEvent::FdWaiterCount(fds[0]), 0);  // 未登记到新代 waiters
    BOOST_CHECK_EQUAL(event->GetStatus(), POLLEVENT_TRIGGER);  // PENDING → 对外映射 TRIGGER

    /* 新对象身份判别：fds[0] 现在是 np[0] 的承载体——应自带 O_NONBLOCK，
     * 且读到的是新 peer 预放的 'N'（MSG_DONTWAIT，无数据不挂）。这组
     * 断言同时证明 dup2 落在正确数字上、旧 'O' 没有混到新对象里。 */
    const int fl_now = ::fcntl(fds[0], F_GETFL, 0);
    BOOST_REQUIRE(fl_now >= 0);
    BOOST_CHECK(fl_now & O_NONBLOCK);
    char seen = 0;
    BOOST_REQUIRE_EQUAL(::recv(fds[0], &seen, 1, MSG_DONTWAIT), 1);
    BOOST_CHECK_EQUAL(seen, new_byte);

    BOOST_REQUIRE(event->CommitPark());        // 消费锁存的 CLOSED 首胜
    BOOST_CHECK_EQUAL(callback_count.load(std::memory_order_acquire), 1);
    BOOST_CHECK_EQUAL(callback_flags.load(std::memory_order_acquire), POLL_EVENT_CLOSED);
    BOOST_CHECK(event->IsFinal());
    BOOST_CHECK_EQUAL(event->GetStatus(), POLLEVENT_FINAL);
    BOOST_CHECK_EQUAL(event->Trigger(POLL_EVENT_CUSTOM), -1);   // 唯一完成，二次触发拒绝

    ::close(np[0]);
    ::close(np[1]);
    ::close(fds[1]);                          // 旧 peer 收尾
    if (::fcntl(fds[0], F_GETFD) >= 0) ::close(fds[0]);   // 新对象收尾
}

BOOST_AUTO_TEST_SUITE_END()
