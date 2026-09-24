#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/DnsResolver.hpp>

#include <bbt/core/thread/Lock.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <netdb.h>

BOOST_AUTO_TEST_SUITE(DnsLifecycle)

BOOST_AUTO_TEST_CASE(t_dns_restarts_after_scheduler_stop) {
    g_scheduler->Start();
    g_scheduler->Stop();
    g_scheduler->Start();

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
    g_scheduler->Stop();
}


BOOST_AUTO_TEST_CASE(t_dns_bounded_wait_keeps_worker_owned) {
    g_scheduler->Start();
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
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_CASE(t_dns_expired_deadline_does_not_enqueue) {
    g_scheduler->Start();
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
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_CASE(t_dns_precancelled_does_not_enqueue) {
    g_scheduler->Start();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int status{-1};
    std::atomic_bool ran{false};
    bbt::coroutine::CancellationSource cancel;
    cancel.RequestCancel();
    bbtco [&]() {
        bbt::coroutine::sync::CombinedWaitOptions options;
        options.cancel = cancel.Token();
        status.store(static_cast<int>(
            bbt::coroutine::detail::DnsResolver::GetInstance()->AwaitBounded(
                [&]() { ran.store(true); }, options)));
        done.Down();
    };
    BOOST_REQUIRE_EQUAL(done.WaitTimeout(2000), 0);
    BOOST_CHECK_EQUAL(status.load(), static_cast<int>(
        bbt::coroutine::sync::CombinedWaitStatus::Cancelled));
    BOOST_CHECK(!ran.load());
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_CASE(t_dns_cancel_while_worker_runs) {
    g_scheduler->Start();
    struct Gate {
        std::mutex mutex;
        std::condition_variable cv;
        bool entered{false};
        bool release{false};
    };
    auto gate = std::make_shared<Gate>();
    bbt::coroutine::CancellationSource cancel;
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int status{-1};
    bbtco [gate, &cancel, &status, &done]() {
        bbt::coroutine::sync::CombinedWaitOptions options;
        options.cancel = cancel.Token();
        auto outcome = bbt::coroutine::detail::DnsResolver::GetInstance()->AwaitBounded(
            [gate]() {
                std::unique_lock<std::mutex> lock(gate->mutex);
                gate->entered = true;
                gate->cv.notify_all();
                gate->cv.wait(lock, [&] { return gate->release; });
            }, options);
        status.store(static_cast<int>(outcome));
        done.Down();
    };
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        entered = gate->cv.wait_for(lock, std::chrono::seconds{2},
                                    [&] { return gate->entered; });
    }
    if (entered)
        cancel.RequestCancel();
    const int finished = entered ? done.WaitTimeout(2000) : -1;
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->release = true;
    }
    gate->cv.notify_all();
    BOOST_CHECK_EQUAL(finished, 0);
    BOOST_CHECK_EQUAL(status.load(), static_cast<int>(
        bbt::coroutine::sync::CombinedWaitStatus::Cancelled));
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
