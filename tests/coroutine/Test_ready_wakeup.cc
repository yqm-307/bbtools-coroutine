#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;
using Clock = std::chrono::steady_clock;

namespace {
// 只暴露已有受保护的内部入口，不修改生产状态或公开 API。
#ifndef BBT_READY_PUBLIC_ONLY
struct ProcesserProbe : Processer {
    using Processer::_Init;
    using Processer::_NotifyRunnable;
    using Processer::_WaitForWork;
};
#endif

void RequireReady(std::future<void>& done)
{
    BOOST_REQUIRE(done.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
    done.get();
}
}

#ifndef BBT_READY_PUBLIC_ONLY
BOOST_AUTO_TEST_CASE(notification_latched_before_wait)
{
    // 独立 P 不运行用户任务；累计256次握手，避免亚毫秒单次阈值。
    ProcesserProbe processer;
    processer._Init();
    const auto begin = Clock::now();
    for (int i = 0; i < 256; ++i) {
        processer._NotifyRunnable();
        processer._WaitForWork();
    }
    BOOST_CHECK(Clock::now() - begin < std::chrono::milliseconds(100));
}
#endif

BOOST_AUTO_TEST_CASE(external_ready_before_sleep_and_parked)
{
    auto* cfg = g_bbt_coroutine_config.get();
    cfg->m_cfg_static_thread_num = 1;
    cfg->m_cfg_stack_size = 65536;
    g_scheduler->Start();

    // 主线程 gate 先允许 Notify 在 register 回调尚未返回时完成，
    // 再放 worker commit yield；覆盖 publish-before-idle，不靠亚毫秒阈值。
    sync::CoWaiter early;
    std::promise<void> entered, release, completed;
    auto registered = entered.get_future();
    auto gate = release.get_future();
    auto done = completed.get_future();
    std::atomic<int> status{-1};
    g_scheduler->RegistCoroutineTask(65536, [&] {
        sync::CombinedWaitOptions options;
        status = static_cast<int>(early.Wait(options, [&] {
            entered.set_value();
            gate.wait();
            return true;
        }));
        completed.set_value();
    });
    RequireReady(registered);
    BOOST_REQUIRE_EQUAL(early.Notify(CoEventValue::From(true)), 0);
    release.set_value();
    RequireReady(done);
    BOOST_CHECK_EQUAL(status.load(), static_cast<int>(sync::CombinedWaitStatus::Completed));
}

BOOST_AUTO_TEST_CASE(concurrent_external_notifications)
{
    auto* cfg = g_bbt_coroutine_config.get();
    cfg->m_cfg_static_thread_num = 4;
    cfg->m_cfg_stack_size = 65536;
    g_scheduler->Start();

    // 四个真实 waiter、四个并发通知线程、重复代次：完成计数必须恰好一次。
    // 主线程的条件变量 gate 保证每轮已登记；首轮增加50ms使 worker 停放。
    std::array<sync::CoWaiter, 4> waiters;
    std::array<std::atomic<int>, 4> registered_round{};
    std::array<std::atomic<int>, 4> completions{};
    std::atomic<int> failures{0};
    std::mutex mutex;
    std::condition_variable changed;
    std::array<std::promise<void>, 4> finished;
    std::array<std::future<void>, 4> futures;
    for (int i = 0; i < 4; ++i) {
        futures[i] = finished[i].get_future();
        g_scheduler->RegistCoroutineTask(65536, [&, i] {
            for (int round = 1; round <= 64; ++round) {
                sync::CombinedWaitOptions options;
                auto result = waiters[i].Wait(options, [&] {
                    { std::lock_guard<std::mutex> lock(mutex); registered_round[i] = round; }
                    changed.notify_all();
                    return true;
                });
                if (result != sync::CombinedWaitStatus::Completed) ++failures;
                ++completions[i];
            }
            finished[i].set_value();
        });
    }
    std::array<std::thread, 4> notifiers;
    const auto begin = Clock::now();
    for (int i = 0; i < 4; ++i) {
        notifiers[i] = std::thread([&, i] {
            for (int round = 1; round <= 64; ++round) {
                std::unique_lock<std::mutex> lock(mutex);
                if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return registered_round[i] >= round; })) {
                    ++failures;
                    return;
                }
                lock.unlock();
                if (round == 1) std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (waiters[i].Notify(CoEventValue::From(round)) != 0) ++failures;
            }
        });
    }
    for (auto& notifier : notifiers) notifier.join();
    for (auto& future : futures) RequireReady(future);
    BOOST_CHECK_EQUAL(failures.load(), 0);
    for (auto& count : completions) BOOST_CHECK_EQUAL(count.load(), 64);
    // 64次周期的累计预算，不对单次OS唤醒作实时保证。旧版需约200ms轮询；
    // 50ms显式停放包含在内，候选有110ms余量，不靠亚毫秒阈值。
    BOOST_CHECK(Clock::now() - begin < std::chrono::milliseconds(160));
}

BOOST_AUTO_TEST_CASE(external_parked_progress)
{
    g_bbt_coroutine_config->m_cfg_static_thread_num = 1;
    g_bbt_coroutine_config->m_cfg_stack_size = 65536;
    g_scheduler->Start();
    sync::CoWaiter waiter;
    std::mutex mutex;
    std::condition_variable changed;
    int registered = -1;
    std::atomic<int> failures{0};
    std::promise<void> completed;
    auto done = completed.get_future();
    g_scheduler->RegistCoroutineTask(65536, [&] {
        for (int i = 0; i < 128; ++i) {
            sync::CombinedWaitOptions options;
            if (waiter.Wait(options, [&] {
                { std::lock_guard<std::mutex> lock(mutex); registered = i; }
                changed.notify_one();
                return true;
            }) != sync::CombinedWaitStatus::Completed) ++failures;
        }
        completed.set_value();
    });
    const auto begin = Clock::now();
    for (int i = 0; i < 128; ++i) {
        std::unique_lock<std::mutex> lock(mutex);
        BOOST_REQUIRE(changed.wait_for(lock, std::chrono::seconds(2), [&] { return registered >= i; }));
        lock.unlock();
        // 与公开诊断复现相同的停放刺激；在计时内，阈值是128次的总预算。
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        BOOST_REQUIRE_EQUAL(waiter.Notify(CoEventValue::From(true)), 0);
    }
    RequireReady(done);
    BOOST_CHECK_EQUAL(failures.load(), 0);
    // 旧版约390ms；候选约140ms，保留110ms OS调度余量，不要求单次实时性。
    BOOST_CHECK(Clock::now() - begin < std::chrono::milliseconds(250));
}
