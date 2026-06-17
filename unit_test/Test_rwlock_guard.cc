#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoRWMutex.hpp>
#include <bbt/coroutine/sync/CoLockGuard.hpp>
using namespace bbt::coroutine::sync;

BOOST_AUTO_TEST_SUITE(RWLockGuardTest)

BOOST_AUTO_TEST_CASE(t_scheduler_start)
{
    g_scheduler->Start();
}

// ============ CoReadLock ============

// 基本读锁 RAII
BOOST_AUTO_TEST_CASE(t_readlock_basic)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [rwlock, &l]()
    {
        CoReadLock guard(rwlock);
        BOOST_TEST(guard.owns_lock());
        BOOST_TEST(guard.owns_lock());
        l.Down();
    };

    l.Wait();
}

// 多 reader 可同时持有读锁
BOOST_AUTO_TEST_CASE(t_readlock_multi_reader)
{
    auto rwlock = CoRWMutex::Create();
    const int nreaders = 5;
    bbt::core::thread::CountDownLatch l{nreaders};
    int readers_inside = 0;
    int max_concurrent = 0;

    for (int i = 0; i < nreaders; ++i) {
        bbtco [rwlock, &readers_inside, &max_concurrent, &l]()
        {
            CoReadLock guard(rwlock);
            readers_inside++;
            if (readers_inside > max_concurrent)
                max_concurrent = readers_inside;
            bbtco_sleep(50);
            readers_inside--;
            l.Down();
        };
    }

    l.Wait();
    BOOST_TEST(max_concurrent >= 2); // 多 reader 应能同时持有
}

// 读锁释放后写锁可获取
BOOST_AUTO_TEST_CASE(t_readlock_releases_for_write)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{1};
    bool write_ok = false;

    bbtco [rwlock, &l, &write_ok]()
    {
        {
            CoReadLock guard(rwlock);
            BOOST_TEST(guard.owns_lock());
        } // 释放读锁

        // 读锁释放后可以获取写锁
        CoWriteLock wguard(rwlock);
        write_ok = true;
        l.Down();
    };

    l.Wait();
    BOOST_TEST(write_ok);
}

// try_to_lock 读锁
BOOST_AUTO_TEST_CASE(t_readlock_try_to_lock)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [rwlock, &l]()
    {
        CoReadLock guard(rwlock, std::try_to_lock);
        BOOST_TEST(guard.owns_lock()); // 初始应成功
        l.Down();
    };

    l.Wait();
}

// try_lock_for 读锁
BOOST_AUTO_TEST_CASE(t_readlock_try_lock_for)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [rwlock, &l]()
    {
        CoReadLock guard(rwlock, std::defer_lock);
        bool ok = guard.try_lock_for(100);
        BOOST_TEST(ok);
        BOOST_TEST(guard.owns_lock());
        l.Down();
    };

    l.Wait();
}

// defer_lock + 手动 lock/unlock
BOOST_AUTO_TEST_CASE(t_readlock_defer_lock)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [rwlock, &l]()
    {
        CoReadLock guard(rwlock, std::defer_lock);
        BOOST_TEST(!guard.owns_lock());

        guard.lock();
        BOOST_TEST(guard.owns_lock());

        guard.unlock();
        BOOST_TEST(!guard.owns_lock());
        l.Down();
    };

    l.Wait();
}

// 移动语义
BOOST_AUTO_TEST_CASE(t_readlock_move)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [rwlock, &l]()
    {
        CoReadLock inner;
        {
            CoReadLock outer(rwlock);
            BOOST_TEST(outer.owns_lock());
            inner = std::move(outer);
            BOOST_TEST(!outer.owns_lock());
        }
        BOOST_TEST(inner.owns_lock());
        l.Down();
    };

    l.Wait();
}

// ============ CoWriteLock ============

// 基本写锁 RAII
BOOST_AUTO_TEST_CASE(t_writelock_basic)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [rwlock, &l]()
    {
        CoWriteLock guard(rwlock);
        BOOST_TEST(guard.owns_lock());
        BOOST_TEST(guard.owns_lock());
        l.Down();
    };

    l.Wait();
}

// 写锁互斥——同时只能有一个写者
BOOST_AUTO_TEST_CASE(t_writelock_mutual_exclusion)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{2};
    int inside = 0;
    int max_inside = 0;

    // 写者 1：持锁一会儿
    bbtco [rwlock, &inside, &max_inside, &l]()
    {
        CoWriteLock guard(rwlock);
        inside = 1;
        if (inside > max_inside) max_inside = inside;
        bbtco_sleep(100);
        inside = 0;
        l.Down();
    };

    // 写者 2：尝试获取（应排队等待）
    bbtco [rwlock, &inside, &max_inside, &l]()
    {
        bbtco_sleep(10);
        CoWriteLock guard(rwlock);
        inside = 1;
        if (inside > max_inside) max_inside = inside;
        bbtco_sleep(10);
        inside = 0;
        l.Down();
    };

    l.Wait();
    BOOST_TEST(max_inside == 1); // 同时最多一个写者
}

// 写锁 try_to_lock
BOOST_AUTO_TEST_CASE(t_writelock_try_to_lock)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [rwlock, &l]()
    {
        CoWriteLock guard(rwlock, std::try_to_lock);
        BOOST_TEST(guard.owns_lock());
        l.Down();
    };

    l.Wait();
}

// 写锁 try_to_lock 冲突
BOOST_AUTO_TEST_CASE(t_writelock_try_to_lock_conflict)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{2};

    bbtco [rwlock, &l]()
    {
        CoWriteLock guard(rwlock);
        bbtco_sleep(200);
        l.Down();
    };

    bbtco [rwlock, &l]()
    {
        bbtco_sleep(10);
        CoWriteLock guard(rwlock, std::try_to_lock);
        BOOST_TEST(!guard.owns_lock()); // 应失败，写锁已被持有
        l.Down();
    };

    l.Wait();
}

// 写锁 try_lock_for
BOOST_AUTO_TEST_CASE(t_writelock_try_lock_for)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [rwlock, &l]()
    {
        CoWriteLock guard(rwlock, std::defer_lock);
        bool ok = guard.try_lock_for(100);
        BOOST_TEST(ok);
        BOOST_TEST(guard.owns_lock());
        l.Down();
    };

    l.Wait();
}

// 写锁 defer + lock/unlock
BOOST_AUTO_TEST_CASE(t_writelock_defer_lock)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [rwlock, &l]()
    {
        CoWriteLock guard(rwlock, std::defer_lock);
        BOOST_TEST(!guard.owns_lock());

        guard.lock();
        BOOST_TEST(guard.owns_lock());

        guard.unlock();
        BOOST_TEST(!guard.owns_lock());
        l.Down();
    };

    l.Wait();
}

// 读写混合场景：读锁释放后写锁可获取
BOOST_AUTO_TEST_CASE(t_read_then_write)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{2};
    int shared = 0;

    bbtco [rwlock, &shared, &l]()
    {
        {
            CoReadLock guard(rwlock);
            BOOST_TEST(shared == 0); // 读时不应有写者
            bbtco_sleep(50);
        }
        l.Down();
    };

    bbtco [rwlock, &shared, &l]()
    {
        CoWriteLock guard(rwlock);
        shared = 42;
        bbtco_sleep(20);
        l.Down();
    };

    l.Wait();
    BOOST_TEST(shared == 42);
}

// 移动语义
BOOST_AUTO_TEST_CASE(t_writelock_move)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [rwlock, &l]()
    {
        CoWriteLock inner;
        {
            CoWriteLock outer(rwlock);
            inner = std::move(outer);
            BOOST_TEST(!outer.owns_lock());
        }
        BOOST_TEST(inner.owns_lock());
        l.Down();
    };

    l.Wait();
}

// 异常安全：异常时析构自动释放锁
BOOST_AUTO_TEST_CASE(t_readlock_exception_safety)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{2};

    bbtco [rwlock, &l]()
    {
        try {
            CoReadLock guard(rwlock);
            throw std::runtime_error("test");
        } catch (...) {}
        l.Down();
    };

    bbtco [rwlock, &l]()
    {
        CoReadLock guard(rwlock);
        BOOST_TEST(guard.owns_lock());
        l.Down();
    };

    l.Wait();
}

// 写锁异常安全
BOOST_AUTO_TEST_CASE(t_writelock_exception_safety)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{2};

    bbtco [rwlock, &l]()
    {
        try {
            CoWriteLock guard(rwlock);
            throw std::runtime_error("test");
        } catch (...) {}
        l.Down();
    };

    bbtco [rwlock, &l]()
    {
        CoWriteLock guard(rwlock);
        BOOST_TEST(guard.owns_lock());
        l.Down();
    };

    l.Wait();
}

// adopt_lock
BOOST_AUTO_TEST_CASE(t_readlock_adopt_lock)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [rwlock, &l]()
    {
        rwlock->RLock();
        {
            CoReadLock guard(rwlock, std::adopt_lock);
            BOOST_TEST(guard.owns_lock());
        }
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_writelock_adopt_lock)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [rwlock, &l]()
    {
        rwlock->WLock();
        {
            CoWriteLock guard(rwlock, std::adopt_lock);
            BOOST_TEST(guard.owns_lock());
        }
        l.Down();
    };

    l.Wait();
}

// 默认构造
BOOST_AUTO_TEST_CASE(t_readlock_default_ctor)
{
    CoReadLock lock;
    BOOST_TEST(!lock.owns_lock());
    BOOST_TEST(!lock);
    BOOST_TEST(lock.mutex() == nullptr);
}

BOOST_AUTO_TEST_CASE(t_writelock_default_ctor)
{
    CoWriteLock lock;
    BOOST_TEST(!lock.owns_lock());
    BOOST_TEST(!lock);
    BOOST_TEST(lock.mutex() == nullptr);
}

// 读锁持有时写锁 try_lock 应失败
BOOST_AUTO_TEST_CASE(t_write_trylock_during_read)
{
    auto rwlock = CoRWMutex::Create();
    bbt::core::thread::CountDownLatch l{2};

    bbtco [rwlock, &l]()
    {
        CoReadLock guard(rwlock);
        bbtco_sleep(100);
        l.Down();
    };

    bbtco [rwlock, &l]()
    {
        bbtco_sleep(10);
        CoWriteLock guard(rwlock, std::try_to_lock);
        BOOST_TEST(!guard.owns_lock()); // 有 reader 时写锁应失败
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_scheduler_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
