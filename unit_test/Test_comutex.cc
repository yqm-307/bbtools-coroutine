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

BOOST_AUTO_TEST_CASE(t_scheudler_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()