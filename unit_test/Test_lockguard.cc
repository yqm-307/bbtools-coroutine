#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
#include <bbt/coroutine/sync/CoLockGuard.hpp>
using namespace bbt::coroutine::sync;

BOOST_AUTO_TEST_SUITE(LockGuardTest)

BOOST_AUTO_TEST_CASE(t_scheduler_start)
{
    g_scheduler->Start();
}

// ============ CoLockGuard ============

// 基本 RAII：构造加锁，析构解锁
BOOST_AUTO_TEST_CASE(t_lockguard_basic)
{
    auto mutex = bbtco_make_comutex();
    int a = 0;
    const int nco_num = 20;
    const int nsec = 500;
    const uint64_t begin = bbt::core::clock::gettime_mono();
    bbt::core::thread::CountDownLatch l{nco_num};

    for (int i = 0; i < nco_num; ++i) {
        bbtco [mutex, &a, begin, nsec, &l]()
        {
            while ((bbt::core::clock::gettime_mono() - begin) < nsec) {
                CoLockGuard<CoMutex> guard(mutex);
                a++;
            }
            l.Down();
        };
    }

    l.Wait();
    BOOST_TEST(a > 0);
}

// CoLockGuard 释放后其他协程可以获取锁
// CoLockGuard 释放后可以重新获取锁
BOOST_AUTO_TEST_CASE(t_lockguard_releases_lock)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [mutex, &l]()
    {
        {
            CoLockGuard<CoMutex> guard(mutex);
        }
        {
            CoLockGuard<CoMutex> guard(mutex);
        }
        l.Down();
    };

    l.Wait();
}
}

// ============ CoUniqueLock ============

// 默认构造：无关联 mutex（不需要协程上下文）
BOOST_AUTO_TEST_CASE(t_unique_default_ctor)
{
    CoUniqueLock<CoMutex> lock;
    BOOST_TEST(!lock.owns_lock());
    BOOST_TEST(!lock.owns_lock());
    BOOST_TEST(lock.mutex() == nullptr);
}

// 正常锁定（在协程内）
BOOST_AUTO_TEST_CASE(t_unique_lock_basic)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [mutex, &l]()
    {
        CoUniqueLock<CoMutex> lock(mutex);
        BOOST_TEST(lock.owns_lock());
        BOOST_TEST(lock.owns_lock());
        BOOST_TEST(lock.mutex() == mutex);
        l.Down();
    };

    l.Wait();
}

// defer_lock + 手动 lock/unlock（在协程内）
BOOST_AUTO_TEST_CASE(t_unique_defer_lock)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [mutex, &l]()
    {
        CoUniqueLock<CoMutex> lock(mutex, std::defer_lock);
        BOOST_TEST(!lock.owns_lock());

        lock.lock();
        BOOST_TEST(lock.owns_lock());

        lock.unlock();
        BOOST_TEST(!lock.owns_lock());
        l.Down();
    };

    l.Wait();
}

// try_to_lock（在协程内）
BOOST_AUTO_TEST_CASE(t_unique_try_to_lock)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [mutex, &l]()
    {
        CoUniqueLock<CoMutex> lock(mutex, std::try_to_lock);
        // 初始状态锁是空闲的，应该成功
        BOOST_TEST(lock.owns_lock());
        l.Down();
    };

    l.Wait();
}

// try_lock_for 超时
BOOST_AUTO_TEST_CASE(t_unique_try_lock_for_timeout)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{2};

    // 协程 A：持有锁一段时间
    bbtco [mutex, &l]()
    {
        CoUniqueLock<CoMutex> lock(mutex);
        BOOST_TEST(lock.owns_lock());
        bbtco_sleep(200); // 持有 200ms
        l.Down();
    };

    // 协程 B：尝试在 50ms 内获取锁（应该超时）
    bbtco [mutex, &l]()
    {
        // 等一会儿确保 A 先获得锁
        bbtco_sleep(10);
        CoUniqueLock<CoMutex> lock(mutex, std::defer_lock);
        bool got = lock.try_lock_for(50);
        BOOST_TEST(!got);
        BOOST_TEST(!lock.owns_lock());
        l.Down();
    };

    l.Wait();
}

// try_lock_for 成功
BOOST_AUTO_TEST_CASE(t_unique_try_lock_for_success)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [mutex, &l]()
    {
        CoUniqueLock<CoMutex> lock(mutex, std::defer_lock);
        bool got = lock.try_lock_for(500);
        BOOST_TEST(got);
        BOOST_TEST(lock.owns_lock());
        l.Down();
    };

    l.Wait();
}

// adopt_lock（在协程内）
BOOST_AUTO_TEST_CASE(t_unique_adopt_lock)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [mutex, &l]()
    {
        mutex->Lock();
        {
            CoUniqueLock<CoMutex> lock(mutex, std::adopt_lock);
            BOOST_TEST(lock.owns_lock());
        } // 析构时 UnLock
        // 验证锁已释放：可以再次获取
        CoUniqueLock<CoMutex> lock2(mutex);
        BOOST_TEST(lock2.owns_lock());
        l.Down();
    };

    l.Wait();
}

// 移动构造（在协程内）
BOOST_AUTO_TEST_CASE(t_unique_move_ctor)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [mutex, &l]()
    {
        CoUniqueLock<CoMutex> lock1(mutex);
        BOOST_TEST(lock1.owns_lock());

        CoUniqueLock<CoMutex> lock2(std::move(lock1));
        BOOST_TEST(!lock1.owns_lock());
        BOOST_TEST(lock1.mutex() == nullptr);
        BOOST_TEST(lock2.owns_lock());
        BOOST_TEST(lock2.mutex() == mutex);
        l.Down();
    };

    l.Wait();
}

// 移动赋值（在协程内）
BOOST_AUTO_TEST_CASE(t_unique_move_assign)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [mutex, &l]()
    {
        CoUniqueLock<CoMutex> lock1(mutex);
        CoUniqueLock<CoMutex> lock2;

        lock2 = std::move(lock1);
        BOOST_TEST(!lock1.owns_lock());
        BOOST_TEST(lock2.owns_lock());
        l.Down();
    };

    l.Wait();
}

// 移动赋值时，目标对象如果持有锁，先释放
BOOST_AUTO_TEST_CASE(t_unique_move_assign_release)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco [mutex, &l]()
    {
        CoUniqueLock<CoMutex> lock1(mutex);
        BOOST_TEST(lock1.owns_lock());

        CoUniqueLock<CoMutex> lock2;
        lock1.unlock(); // 先释放
        lock2 = std::move(lock1);
        BOOST_TEST(!lock1.owns_lock());
        l.Down();
    };

    l.Wait();
}

// 析构释放锁：scope exit 自动 unlock
BOOST_AUTO_TEST_CASE(t_unique_destructor_unlocks)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{2};

    bbtco [mutex, &l]()
    {
        {
            CoUniqueLock<CoMutex> lock(mutex);
            BOOST_TEST(lock.owns_lock());
        } // 析构释放锁
        l.Down();
    };

    bbtco [mutex, &l]()
    {
        CoUniqueLock<CoMutex> lock(mutex);
        BOOST_TEST(lock.owns_lock());
        l.Down();
    };

    l.Wait();
}

// 异常安全：异常抛出时析构自动释放锁
BOOST_AUTO_TEST_CASE(t_unique_exception_safety)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{2};

    bbtco [mutex, &l]()
    {
        try {
            CoUniqueLock<CoMutex> lock(mutex);
            throw std::runtime_error("test exception");
        } catch (...) {
            // lock 析构已释放锁
        }
        l.Down();
    };

    bbtco [mutex, &l]()
    {
        CoUniqueLock<CoMutex> lock(mutex);
        BOOST_TEST(lock.owns_lock());
        l.Down();
    };

    l.Wait();
}

// 移动后释放：移动到的对象析构时正确释放锁
BOOST_AUTO_TEST_CASE(t_unique_move_then_release)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{2};

    bbtco [mutex, &l]()
    {
        CoUniqueLock<CoMutex> inner;
        {
            CoUniqueLock<CoMutex> outer(mutex);
            inner = std::move(outer);
        } // outer 析构（已不持有锁，安全）
        BOOST_TEST(inner.owns_lock());
        l.Down();
    }; // inner 析构释放锁

    // 验证锁已释放：另一个协程可以获取
    bbtco [mutex, &l]()
    {
        CoUniqueLock<CoMutex> lock(mutex);
        BOOST_TEST(lock.owns_lock());
        l.Down();
    };

    l.Wait();
}

// CoLockGuard 异常安全
BOOST_AUTO_TEST_CASE(t_lockguard_exception_safety)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{2};

    bbtco [mutex, &l]()
    {
        try {
            CoLockGuard<CoMutex> guard(mutex);
            throw std::runtime_error("test");
        } catch (...) {}
        l.Down();
    };

    bbtco [mutex, &l]()
    {
        CoLockGuard<CoMutex> guard(mutex);
        BOOST_TEST(true);
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_scheduler_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
