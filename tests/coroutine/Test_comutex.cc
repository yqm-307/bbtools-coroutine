#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>

/**
 * 进程寿命模型：runtime 只初始化一次，重复 Start 抛 std::logic_error。
 * 每个测试文件就是一个可执行，这里把用例内的 Start() 收敛为进程内一次初始化。
 */
namespace {
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, []() { bbt::coroutine::detail::Scheduler::GetInstance()->Start(); });
}

/**
 * 截止时间保护的让权自旋：仅在协程内使用，等待某个已发生的事实变得可见。
 * 超时返回 false 交由调用方断言失败；不引入正确性依赖的固定等待。
 */
bool SpinUntilCo(const std::function<bool()>& ready, int timeout_ms)
{
    auto deadline = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(timeout_ms));
    while (!ready()) {
        if (bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(deadline))
            return false;
        bbtco_yield;
    }
    return true;
}

/**
 * 主线程有界等待收口原子计数：等待超时判失败，不无限阻塞测试进程。
 */
void WaitCountBounded(std::atomic_int& count, int target, int timeout_ms)
{
    auto deadline = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(timeout_ms));
    while (count.load() < target) {
        if (bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(deadline)) {
            BOOST_ERROR("wait count timed out");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
} // namespace

using namespace bbt::coroutine::sync;

BOOST_AUTO_TEST_SUITE(CoMutexTest)

/**
 * Suite 级别：先 Start，结束 Stop
 */
BOOST_AUTO_TEST_CASE(t_scheduler_start)
{
    EnsureRuntime();
}

/**
 * ============ 已有测试 ============
 */

/**
 * 普通加解锁
 */
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
        bbtco[mutex, &a, &b, begin, &l]()
        {
            while ((bbt::core::clock::gettime_mono() - begin) < nsec) {
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

/**
 * 尝试加解锁（使用 TryLock(5ms) 竞争，避免 1ms 产生过多 timeout 事件）
 */
BOOST_AUTO_TEST_CASE(t_try_lock)
{
    auto mutex = bbtco_make_comutex();
    const int nco_num = 100;

    /**
     * 缩短到 500ms
     */
    const int nsec = 500;
    int a = 0;
    int b = 0;
    int total_acquired = 0;

    const uint64_t begin = bbt::core::clock::gettime_mono();
    bbt::core::thread::CountDownLatch l{nco_num};

    for (int i = 0; i < nco_num; ++i) {
        bbtco[mutex, &a, &b, begin, &l, &total_acquired]()
        {
            while ((bbt::core::clock::gettime_mono() - begin) < nsec) {
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

/**
 * ============ P0 新测试 ============
 */

/**
 * Lock → UnLock → Lock 正常重入
 */
BOOST_AUTO_TEST_CASE(t_reentry_lock_unlock_lock)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco[mutex, &l]()
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

/**
 * 多次 Lock/UnLock 循环
 */
BOOST_AUTO_TEST_CASE(t_reentry_lock_unlock_loop)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco[mutex, &l]()
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

/**
 * TryLock() 立即返回 -1 当锁已被占用
 */
BOOST_AUTO_TEST_CASE(t_trylock_immediate_fails_when_locked)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch holder_got_lock{1};
    bbt::core::thread::CountDownLatch holder_done{1};
    bbt::core::thread::CountDownLatch waiter_done{1};
    int trylock_result = 0;

    bbtco[mutex, &holder_got_lock, &holder_done]()
    {
        mutex->Lock();
        holder_got_lock.Down();
        bbtco_sleep(200);
        mutex->UnLock();
        holder_done.Down();
    };

    bbtco[mutex, &holder_got_lock, &waiter_done, &trylock_result]()
    {
        holder_got_lock.Wait();
        trylock_result = mutex->TryLock();
        BOOST_TEST(trylock_result == -1);
        waiter_done.Down();
    };

    holder_done.Wait();
    waiter_done.Wait();
}

/**
 * TryLock(ms) 超时返回
 */
BOOST_AUTO_TEST_CASE(t_trylock_timeout_returns_on_timeout)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch holder_got_lock{1};
    bbt::core::thread::CountDownLatch holder_done{1};
    bbt::core::thread::CountDownLatch waiter_done{1};
    int trylock_result = -999;

    bbtco[mutex, &holder_got_lock, &holder_done]()
    {
        mutex->Lock();
        holder_got_lock.Down();
        bbtco_sleep(300);
        mutex->UnLock();
        holder_done.Down();
    };

    bbtco[mutex, &holder_got_lock, &waiter_done, &trylock_result]()
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

/**
 * TryLock(ms) 超时后锁仍可用
 */
BOOST_AUTO_TEST_CASE(t_trylock_timeout_no_dirty_state)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};
    int ok = 0;

    bbtco[mutex, &l, &ok]()
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

/**
 * TryLock() 空闲时成功
 */
BOOST_AUTO_TEST_CASE(t_trylock_immediate_succeeds_when_free)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco[mutex, &l]()
    {
        int ret = mutex->TryLock();
        BOOST_TEST(ret == 0);
        mutex->UnLock();
        l.Down();
    };

    l.Wait();
}

/**
 * TryLock(ms) 空闲时成功
 */
BOOST_AUTO_TEST_CASE(t_trylock_ms_succeeds_when_free)
{
    auto mutex = bbtco_make_comutex();
    bbt::core::thread::CountDownLatch l{1};

    bbtco[mutex, &l]()
    {
        int ret = mutex->TryLock(1000);
        BOOST_TEST(ret == 0);
        mutex->UnLock();
        l.Down();
    };

    l.Wait();
}

/**
 * =================== 锁外发布保护：解锁交接 ===================
 * 等待/唤醒热路径改造（首胜声明保留 + 已挂起完成的锁外延迟唤醒）的确定性回归。
 * 阶段证明来自尝试标志与截止时间自旋，不以 sleep 作为正确性依据。
 */

/**
 * UnLock 交接唯一唤醒且状态可见：持锁方在两名竞争者发起 Lock 后写入共享
 * 值再解锁，恰有一人先获锁，owner CAS 保证任一时刻至多一个持有者，
 * 解锁前的写入对后继持有者可见。
 * 抓错：解锁丢失唤醒（后继挂死）、双交付（两人同时持锁，CAS 失败）、
 * 交接可见性破坏（读到解锁前的旧值）、恢复后复查循环活锁。
 * 断言全部在主线程做出。
 */
BOOST_AUTO_TEST_CASE(t_unlock_handoff_single_wake_payload_visible)
{
    struct CaseState {
        CoMutex::SPtr mutex{bbtco_make_comutex()};
        std::atomic_int x{0};
        std::atomic_int attempts{0};
        std::atomic_int acquired{0};
        std::atomic_int owner{0};
        std::atomic_int done{0};
        std::atomic_int stage_attempts_ok{0};
        std::atomic_int stage_done_ok{0};
        std::atomic_int prev1{-999};
        std::atomic_int prev2{-999};
        std::atomic_int x1{0};
        std::atomic_int x2{0};
    };

    auto state = std::make_shared<CaseState>();

    bbtco[state]()
    {
        state->mutex->Lock();
        state->x.store(42);

        bbtco[state]()
        {
            state->attempts.fetch_add(1);
            state->mutex->Lock();
            state->prev1.store(state->owner.exchange(1));
            state->x1.store(state->x.load());
            state->acquired.fetch_add(1);
            state->owner.store(0);
            state->mutex->UnLock();
            state->done.fetch_add(1);
        };

        bbtco[state]()
        {
            state->attempts.fetch_add(1);
            state->mutex->Lock();
            state->prev2.store(state->owner.exchange(2));
            state->x2.store(state->x.load());
            state->acquired.fetch_add(1);
            state->owner.store(0);
            state->mutex->UnLock();
            state->done.fetch_add(1);
        };

        /**
         * 阶段门槛：两名竞争者都已发起 Lock 尝试后再解锁；
         * 竞争者未挂起时直接获锁、已挂起时由解锁唤醒，两种时序结果一致
         */
        state->stage_attempts_ok.store(SpinUntilCo([&state] { return state->attempts.load() == 2; }, 3000) ? 1 : 0);
        state->mutex->UnLock();
        state->stage_done_ok.store(SpinUntilCo([&state] { return state->done.load() == 2; }, 3000) ? 1 : 0);
    };

    WaitCountBounded(state->done, 2, 8000);

    BOOST_CHECK_EQUAL(state->stage_attempts_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->stage_done_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->prev1.load(), 0);
    BOOST_CHECK_EQUAL(state->prev2.load(), 0);
    BOOST_CHECK_EQUAL(state->x1.load(), 42);
    BOOST_CHECK_EQUAL(state->x2.load(), 42);
    BOOST_CHECK_EQUAL(state->acquired.load(), 2);
    BOOST_CHECK_EQUAL(state->owner.load(), 0);
}

/**
 * TryLock(ms) 超时留下的失效 waiter 不得吞掉解锁唤醒：超时收口后新来的
 * Lock 竞争者必须在持锁方解锁后获得锁。
 * 抓错：_NotifyOne 停在已超时事件上（后继挂死）、失效事件占用唤醒名额。
 * 公开 API 无法证明竞争者已挂起：未挂起时退化为直接获锁，两种时序结果一致。
 * 断言全部在主线程做出。
 */
BOOST_AUTO_TEST_CASE(t_trylock_timeout_dead_waiter_skipped)
{
    struct CaseState {
        CoMutex::SPtr mutex{bbtco_make_comutex()};
        std::atomic_int b_ret{-999};
        std::atomic_int b_done{0};
        std::atomic_int c_acquired{0};
        std::atomic_int done{0};
        std::atomic_int stage_b_ok{0};
        std::atomic_int stage_c_ok{0};
    };

    auto state = std::make_shared<CaseState>();

    bbtco[state]()
    {
        state->mutex->Lock();

        bbtco[state]()
        {
            state->b_ret.store(state->mutex->TryLock(60));
            state->b_done.store(1);
        };

        state->stage_b_ok.store(SpinUntilCo([&state] { return state->b_done.load() == 1; }, 3000) ? 1 : 0);

        bbtco[state]()
        {
            state->mutex->Lock();
            state->c_acquired.store(1);
            state->mutex->UnLock();
            state->done.fetch_add(1);
        };

        state->mutex->UnLock();
        state->stage_c_ok.store(SpinUntilCo([&state] { return state->done.load() == 1; }, 3000) ? 1 : 0);
    };

    WaitCountBounded(state->done, 1, 8000);

    BOOST_CHECK_EQUAL(state->stage_b_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->b_ret.load(), 1);
    BOOST_CHECK_EQUAL(state->stage_c_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->c_acquired.load(), 1);
}

BOOST_AUTO_TEST_CASE(t_scheudler_end) {}

BOOST_AUTO_TEST_SUITE_END()
