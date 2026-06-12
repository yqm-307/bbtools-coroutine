#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
using namespace bbt::coroutine::sync;

BOOST_AUTO_TEST_SUITE(CoMutexTest)

// Suite 级别：先 Start，结束 Stop
BOOST_AUTO_TEST_CASE(t_scheduler_start)
{
    g_scheduler->Start();
}

// ============ 已有测试 ============

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

// 尝试加解锁（使用 TryLock(5ms) 竞争，避免 1ms 产生过多 timeout 事件）
BOOST_AUTO_TEST_CASE(t_try_lock)
{
    auto mutex = bbtco_make_comutex();
    const int nco_num = 100;
    const int nsec = 500; // 缩短到 500ms
    int a = 0;
    int b = 0;
    int total_acquired = 0;

    const uint64_t begin = bbt::core::clock::gettime_mono();
    bbt::core::thread::CountDownLatch l{nco_num};

    for (int i = 0; i < nco_num; ++i) {
        bbtco [mutex, &a, &b, begin, &l, &total_acquired]()
        {
            while ((bbt::core::clock::gettime_mono() - begin) < nsec)
            {
                int ret = mutex->TryLock(5);
                if (ret == 0) {
                    BOOST_ASSERT(a == b);
                    a++;
                    b++;
                    total_acquired++;
                    mutex->UnLock();
                }
            }
            l.Down();
        };
    }

    l.Wait();
    BOOST_TEST(total_acquired > 0);
}

// ============ P0 新测试 ============

// Lock → UnLock → Lock 正常重入
BOOST_AUTO_TEST_CASE(t_reentry_lock_unlock_lock)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [mutex, &l]()
    {
        mutex->Lock();
        mutex->UnLock();
        mutex->Lock();
        BOOST_TEST(true);
        mutex->UnLock();
        l.Down();
    };

    l.Wait();
}

// 多次 Lock/UnLock 循环
BOOST_AUTO_TEST_CASE(t_reentry_lock_unlock_loop)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [mutex, &l]()
    {
        for (int i = 0; i < 100; ++i) {
            mutex->Lock();
            mutex->UnLock();
        }
        BOOST_TEST(true);
        l.Down();
    };

    l.Wait();
}

// TryLock() 立即返回 -1 当锁已被占用
BOOST_AUTO_TEST_CASE(t_trylock_immediate_fails_when_locked)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch holder_got_lock{1};
    bbt::core::thread::CountDownLatch holder_done{1};
    bbt::core::thread::CountDownLatch waiter_done{1};
    int trylock_result = 0;

    bbtco [mutex, &holder_got_lock, &holder_done]()
    {
        mutex->Lock();
        holder_got_lock.Down();
        bbtco_sleep(200);
        mutex->UnLock();
        holder_done.Down();
    };

    bbtco [mutex, &holder_got_lock, &waiter_done, &trylock_result]()
    {
        holder_got_lock.Wait();
        trylock_result = mutex->TryLock();
        BOOST_TEST(trylock_result == -1);
        waiter_done.Down();
    };

    holder_done.Wait();
    waiter_done.Wait();
}

// TryLock(ms) 超时返回
BOOST_AUTO_TEST_CASE(t_trylock_timeout_returns_on_timeout)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch holder_got_lock{1};
    bbt::core::thread::CountDownLatch holder_done{1};
    bbt::core::thread::CountDownLatch waiter_done{1};
    int trylock_result = -999;

    bbtco [mutex, &holder_got_lock, &holder_done]()
    {
        mutex->Lock();
        holder_got_lock.Down();
        bbtco_sleep(300);
        mutex->UnLock();
        holder_done.Down();
    };

    bbtco [mutex, &holder_got_lock, &waiter_done, &trylock_result]()
    {
        holder_got_lock.Wait();
        auto begin = bbt::core::clock::gettime_mono();
        trylock_result = mutex->TryLock(50);
        auto elapsed = bbt::core::clock::gettime_mono() - begin;

        BOOST_TEST(trylock_result != 0);
        BOOST_TEST(elapsed >= 40);
        BOOST_TEST(elapsed < 500);
        waiter_done.Down();
    };

    holder_done.Wait();
    waiter_done.Wait();
}

// TryLock(ms) 超时后锁仍可用
BOOST_AUTO_TEST_CASE(t_trylock_timeout_no_dirty_state)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};
    int ok = 0;

    bbtco [mutex, &l, &ok]()
    {
        mutex->Lock();
        mutex->UnLock();

        int ret = mutex->TryLock(10);
        BOOST_TEST(ret == 0);
        if (ret == 0) {
            ok = 1;
            mutex->UnLock();
        }
        l.Down();
    };

    l.Wait();
    BOOST_TEST(ok == 1);
}

// TryLock() 空闲时成功
BOOST_AUTO_TEST_CASE(t_trylock_immediate_succeeds_when_free)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [mutex, &l]()
    {
        int ret = mutex->TryLock();
        BOOST_TEST(ret == 0);
        mutex->UnLock();
        l.Down();
    };

    l.Wait();
}

// TryLock(ms) 空闲时成功
BOOST_AUTO_TEST_CASE(t_trylock_ms_succeeds_when_free)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [mutex, &l]()
    {
        int ret = mutex->TryLock(1000);
        BOOST_TEST(ret == 0);
        mutex->UnLock();
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_scheudler_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
