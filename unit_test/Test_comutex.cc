#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
using namespace bbt::coroutine::sync;

BOOST_AUTO_TEST_SUITE(CoMutexTest)

BOOST_AUTO_TEST_CASE(t_scheduler_start)
{
    g_scheduler->Start();
}

// 普通加解锁
BOOST_AUTO_TEST_CASE(t_lock_unlock)
{
    auto mutex = bbtco_make_comutex();
    const int nco_num = 100;
    const int nsec = 2000;
    int a = 0;
    int b = 0;


    const uint64_t begin = bbt::core::clock::gettime_mono();
    bbt::core::thread::CountDownLatch l{nco_num};

    for (int i = 0; i < nco_num; ++i) {
        bbtco [mutex, &a, &b, begin, &l]()
        {
            while ((bbt::core::clock::gettime_mono() - begin) < nsec)
            {
                mutex->Lock();
                BOOST_ASSERT(a == b);
                a++;
                b++;
                mutex->UnLock();
            }

            l.Down();
        };
    }

    l.Wait();
}

// 尝试加解锁
BOOST_AUTO_TEST_CASE(t_try_lock)
{
    auto mutex = bbtco_make_comutex();
    const int nco_num = 100;
    const int nsec = 2000;
    int a = 0;
    int b = 0;

    const uint64_t begin = bbt::core::clock::gettime_mono();
    bbt::core::thread::CountDownLatch l{nco_num};

    for (int i = 0; i < nco_num; ++i) {
        bbtco [mutex, &a, &b, begin, &l]()
        {
            while ((bbt::core::clock::gettime_mono() - begin) < nsec)
            {
                if (mutex->TryLock(1) == 0) {
                    BOOST_ASSERT(a == b);
                    a++;
                    b++;
                    mutex->UnLock();
                }
            }

            l.Down();
        };
    }

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_try_lock_timeout_result)
{
    auto mutex = bbtco_make_comutex();
    std::atomic_bool locked{false};
    std::atomic_int result{-2};
    std::atomic_int elapsed_ms{0};
    bbt::core::thread::CountDownLatch latch{2};

    bbtco [mutex, &locked, &latch]() {
        mutex->Lock();
        locked = true;
        bbtco_sleep(80);
        mutex->UnLock();
        latch.Down();
    };

    bbtco [mutex, &locked, &result, &elapsed_ms, &latch]() {
        while (!locked.load()) {
            bbtco_sleep(1);
        }

        auto begin = bbt::core::clock::gettime();
        result = mutex->TryLock(20);
        auto end = bbt::core::clock::gettime();
        elapsed_ms = static_cast<int>(end - begin);
        latch.Down();
    };

    latch.Wait();

    BOOST_CHECK_EQUAL(result.load(), 1);
    BOOST_CHECK_GE(elapsed_ms.load(), 20);
}

BOOST_AUTO_TEST_CASE(t_scheudler_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()