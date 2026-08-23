#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>

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
        if (!m_started)
            return;
        m_scheduler.Stop();
        m_started = false;
    }

private:
    Scheduler& m_scheduler;
    bool m_started{false};
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
    scheduler->Start();
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

BOOST_AUTO_TEST_SUITE_END()
