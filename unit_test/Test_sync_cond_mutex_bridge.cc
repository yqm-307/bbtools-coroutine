#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>

using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE(SyncCondMutexBridgeTest)

BOOST_AUTO_TEST_CASE(t_start_scheduler)
{
    g_scheduler->Start();
}

BOOST_AUTO_TEST_CASE(t_cocond_notify_one_skips_expired_waiter)
{
    auto cond = sync::CoCond::Create();
    std::atomic_int timeout_result{-2};
    std::atomic_int wake_result{-2};
    bbt::core::thread::CountDownLatch latch{2};

    bbtco [cond, &timeout_result, &latch]() {
        timeout_result = cond->WaitFor(20);
        latch.Down();
    };

    bbtco [cond, &wake_result, &latch]() {
        wake_result = cond->Wait();
        latch.Down();
    };

    bbtco [cond]() {
        bbtco_sleep(60);
        BOOST_CHECK_EQUAL(cond->NotifyOne(), 0);
    };

    latch.Wait();

    BOOST_CHECK_EQUAL(timeout_result.load(), 1);
    BOOST_CHECK_EQUAL(wake_result.load(), 0);
}

BOOST_AUTO_TEST_CASE(t_comutex_non_owner_unlock_does_not_release_lock)
{
    auto mutex = bbtco_make_comutex();
    std::atomic_bool owner_locked{false};
    std::atomic_int intruder_try_result{-2};
    std::atomic_int before_owner_release{-2};
    std::atomic_int after_owner_release{-2};
    bbt::core::thread::CountDownLatch latch{3};

    bbtco [mutex, &owner_locked, &latch]() {
        mutex->Lock();
        owner_locked = true;
        bbtco_sleep(80);
        mutex->UnLock();
        latch.Down();
    };

    bbtco [mutex, &owner_locked, &intruder_try_result, &latch]() {
        while (!owner_locked.load()) {
            bbtco_sleep(1);
        }

        mutex->UnLock();
        intruder_try_result = mutex->TryLock();
        if (intruder_try_result.load() == 0) {
            mutex->UnLock();
        }
        latch.Down();
    };

    bbtco [mutex, &owner_locked, &before_owner_release, &after_owner_release, &latch]() {
        while (!owner_locked.load()) {
            bbtco_sleep(1);
        }

        bbtco_sleep(20);
        before_owner_release = mutex->TryLock();
        bbtco_sleep(100);
        after_owner_release = mutex->TryLock();
        if (after_owner_release.load() == 0) {
            mutex->UnLock();
        }
        latch.Down();
    };

    latch.Wait();

    BOOST_CHECK_EQUAL(intruder_try_result.load(), -1);
    BOOST_CHECK_EQUAL(before_owner_release.load(), -1);
    BOOST_CHECK_EQUAL(after_owner_release.load(), 0);
}

BOOST_AUTO_TEST_CASE(t_stop_scheduler)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
