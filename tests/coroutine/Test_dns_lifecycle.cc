#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/DnsResolver.hpp>

#include <bbt/core/thread/Lock.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <thread>

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

BOOST_AUTO_TEST_SUITE(DnsLifecycle)

BOOST_AUTO_TEST_CASE(t_dns_resolves_after_start) {
    EnsureRuntime();

    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int rc{-1};
    bbtco [&]() {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_flags = AI_NUMERICHOST;
        addrinfo* result = nullptr;
        rc.store(::getaddrinfo("127.0.0.1", "80", &hints, &result));
        if (result != nullptr)
            ::freeaddrinfo(result);
        done.Down();
    };
    BOOST_REQUIRE_EQUAL(done.WaitTimeout(2000), 0);
    BOOST_CHECK_EQUAL(rc.load(), 0);
}


BOOST_AUTO_TEST_CASE(t_dns_bounded_wait_keeps_worker_owned) {
    EnsureRuntime();
    struct Gate {
        std::mutex mutex;
        std::condition_variable cv;
        bool release{false};
    };
    auto gate = std::make_shared<Gate>();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int status{-1};
    bbtco [gate, &done, &status]() {
        bbt::coroutine::sync::CombinedWaitOptions options;
        options.deadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds{100};
        auto result = bbt::coroutine::detail::DnsResolver::GetInstance()->AwaitBounded(
            [gate]() {
                std::unique_lock<std::mutex> lock(gate->mutex);
                gate->cv.wait(lock, [&] { return gate->release; });
            }, options);
        status.store(static_cast<int>(result));
        done.Down();
    };
    BOOST_REQUIRE_EQUAL(done.WaitTimeout(2000), 0);
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->release = true;
    }
    gate->cv.notify_all();
    BOOST_CHECK_EQUAL(status.load(), static_cast<int>(bbt::coroutine::sync::CombinedWaitStatus::TimedOut));
}

BOOST_AUTO_TEST_CASE(t_dns_expired_deadline_does_not_enqueue) {
    EnsureRuntime();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int status{-1};
    std::atomic_bool ran{false};
    bbtco [&]() {
        bbt::coroutine::sync::CombinedWaitOptions options;
        options.deadline = std::chrono::steady_clock::now() -
                           std::chrono::seconds{1};
        status.store(static_cast<int>(
            bbt::coroutine::detail::DnsResolver::GetInstance()->AwaitBounded(
                [&]() { ran.store(true); }, options)));
        done.Down();
    };
    BOOST_REQUIRE_EQUAL(done.WaitTimeout(2000), 0);
    BOOST_CHECK_EQUAL(status.load(), static_cast<int>(
        bbt::coroutine::sync::CombinedWaitStatus::TimedOut));
    BOOST_CHECK(!ran.load());
}


/* DNS 协程级取消（替代已删除的 CancellationSource 案例）：work 正阻塞在 DNS
 * worker 上时对等待协程 RequestCancel——首胜为 Cancelled；worker 仍归本池所有，
 * 释放后不崩（与 t_dns_bounded_wait_keeps_worker_owned 同口径的实名断言）。 */
BOOST_AUTO_TEST_CASE(t_dns_coroutine_cancel_while_worker_runs) {
    EnsureRuntime();
    struct Gate {
        std::mutex mutex;
        std::condition_variable cv;
        bool entered{false};
        bool release{false};
    };
    auto gate = std::make_shared<Gate>();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int status{-1};
    std::atomic<bbt::coroutine::detail::Coroutine*> co_p{nullptr};

    bbtco [gate, &done, &status, &co_p]() {
        co_p.store(g_bbt_tls_coroutine_co);
        bbt::coroutine::sync::CombinedWaitOptions options;
        options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        status.store(static_cast<int>(
            bbt::coroutine::detail::DnsResolver::GetInstance()->AwaitBounded(
                [gate]() {
                    std::unique_lock<std::mutex> lock(gate->mutex);
                    gate->entered = true;
                    gate->cv.wait(lock, [&] { return gate->release; });
                }, options)));
        done.Down();
    };

    /* 主线程侧有界等待：协程真正挂起在 DNS 等待上（CO_SUSPEND）后再取消 */
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{3000};
    while ((co_p.load() == nullptr ||
            co_p.load()->GetStatus() != bbt::coroutine::detail::CoroutineStatus::CO_SUSPEND) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    BOOST_REQUIRE(co_p.load() != nullptr);
    BOOST_REQUIRE(co_p.load()->GetStatus() == bbt::coroutine::detail::CoroutineStatus::CO_SUSPEND);

    co_p.load()->RequestCancel();
    BOOST_REQUIRE_EQUAL(done.WaitTimeout(5000), 0);
    BOOST_CHECK_EQUAL(status.load(),
                      static_cast<int>(bbt::coroutine::sync::CombinedWaitStatus::Cancelled));

    /* 晚到的 work 仍会完成：释放 worker，证明池仍持有它能收尾 */
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->release = true;
    }
    gate->cv.notify_all();
}

/* DNS 域预取消（进入等待前已请求取消，对应已删除 CancellationToken 的
 * 「预取消不入队」路径）：入口检查必须直接收敛为 Cancelled，且不把 work
 * 交给 DNS worker。r1 指出 DNS 替代覆盖缺少该路径的明示。 */
BOOST_AUTO_TEST_CASE(t_dns_pre_cancel_before_await_returns_cancelled) {
    EnsureRuntime();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int status{-2};
    std::atomic_bool ran{false};
    bbtco [&]() {
        g_bbt_tls_coroutine_co->RequestCancel();   // 先请求取消，再进入等待
        bbt::coroutine::sync::CombinedWaitOptions options;
        options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        status.store(static_cast<int>(
            bbt::coroutine::detail::DnsResolver::GetInstance()->AwaitBounded(
                [&]() { ran.store(true); }, options)));
        done.Down();
    };
    BOOST_REQUIRE_EQUAL(done.WaitTimeout(5000), 0);
    BOOST_CHECK_EQUAL(status.load(), static_cast<int>(
        bbt::coroutine::sync::CombinedWaitStatus::Cancelled));
    BOOST_CHECK(!ran.load());
}

BOOST_AUTO_TEST_SUITE_END()
