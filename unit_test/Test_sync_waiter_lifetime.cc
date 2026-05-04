#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>

using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE(SyncWaiterLifetimeTest)

BOOST_AUTO_TEST_CASE(t_start_scheduler)
{
    g_scheduler->Start();
}

BOOST_AUTO_TEST_CASE(t_cond_notify_all_skips_expired_waiters)
{
    auto cond = sync::CoCond::Create();
    std::atomic_int timeout_result{-2};
    std::atomic_int wake_a{-2};
    std::atomic_int wake_b{-2};
    bbt::core::thread::CountDownLatch latch{3};

    bbtco [cond, &timeout_result, &latch]() {
        timeout_result = cond->WaitFor(20);
        latch.Down();
    };

    bbtco [cond, &wake_a, &latch]() {
        wake_a = cond->Wait();
        latch.Down();
    };

    bbtco [cond, &wake_b, &latch]() {
        wake_b = cond->Wait();
        latch.Down();
    };

    bbtco [cond]() {
        bbtco_sleep(60);
        cond->NotifyAll();
    };

    latch.Wait();

    BOOST_CHECK_EQUAL(timeout_result.load(), 1);
    BOOST_CHECK_EQUAL(wake_a.load(), 0);
    BOOST_CHECK_EQUAL(wake_b.load(), 0);
}

BOOST_AUTO_TEST_CASE(t_cond_destroy_flushes_pending_waiter)
{
    auto cond = sync::CoCond::Create();
    auto* cond_raw = cond.get();
    std::atomic_bool waiter_started{false};
    std::atomic_int wait_result{-2};
    bbt::core::thread::CountDownLatch latch{1};

    bbtco [cond_raw, &waiter_started, &wait_result, &latch]() {
        waiter_started = true;
        wait_result = cond_raw->Wait();
        latch.Down();
    };

    bbtco [&cond, &waiter_started]() {
        while (!waiter_started.load()) {
            bbtco_sleep(1);
        }

        bbtco_sleep(20);
        cond.reset();
    };

    latch.Wait();

    BOOST_CHECK_NE(wait_result.load(), -2);
}

BOOST_AUTO_TEST_CASE(t_cond_repeated_notify_is_safe_after_completion)
{
    auto cond = sync::CoCond::Create();
    std::atomic_int wake_result{-2};
    bbt::core::thread::CountDownLatch latch{1};

    bbtco [cond, &wake_result, &latch]() {
        wake_result = cond->Wait();
        latch.Down();
    };

    bbtco [cond]() {
        bbtco_sleep(20);
        BOOST_CHECK_EQUAL(cond->NotifyOne(), 0);
        BOOST_CHECK_NE(cond->NotifyOne(), 0);
        cond->NotifyAll();
    };

    latch.Wait();

    BOOST_CHECK_EQUAL(wake_result.load(), 0);
}

BOOST_AUTO_TEST_CASE(t_mutex_timeout_waiter_does_not_block_next_waiter)
{
    auto mutex = bbtco_make_comutex();
    std::atomic_bool owner_locked{false};
    std::atomic_int timeout_result{-2};
    std::atomic_int late_waiter_result{-2};
    bbt::core::thread::CountDownLatch latch{3};

    bbtco [mutex, &owner_locked, &latch]() {
        mutex->Lock();
        owner_locked = true;
        bbtco_sleep(80);
        mutex->UnLock();
        latch.Down();
    };

    bbtco [mutex, &owner_locked, &timeout_result, &latch]() {
        while (!owner_locked.load()) {
            bbtco_sleep(1);
        }

        timeout_result = mutex->TryLock(20);
        latch.Down();
    };

    bbtco [mutex, &owner_locked, &late_waiter_result, &latch]() {
        while (!owner_locked.load()) {
            bbtco_sleep(1);
        }

        bbtco_sleep(30);
        late_waiter_result = mutex->TryLock(200);
        if (late_waiter_result.load() == 0) {
            mutex->UnLock();
        }
        latch.Down();
    };

    latch.Wait();

    BOOST_CHECK_EQUAL(timeout_result.load(), 1);
    BOOST_CHECK_EQUAL(late_waiter_result.load(), 0);
}

BOOST_AUTO_TEST_CASE(t_stop_scheduler)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
