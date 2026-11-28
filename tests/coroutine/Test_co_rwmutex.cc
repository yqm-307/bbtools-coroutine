#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <mutex>

#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <memory>
#include <thread>

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
} // namespace

namespace bbt::coroutine::sync {

class CoRWMutexTestAccess {
public:
    static void SetWaitCallbacks(
        CoRWMutex& rwlock, const std::function<void()>& writer_queued, const std::function<void()>& reader_blocked)
    {
        rwlock.m_writer_queued_callback = writer_queued;
        rwlock.m_reader_blocked_callback = reader_blocked;
    }
};

} // namespace bbt::coroutine::sync

/**
 * 截止时间保护的让权自旋：仅在协程内使用，等待某个已发生的事实变得可见。
 * 超时返回 false 交由调用方断言失败；不引入正确性依赖的固定等待。
 */
static bool SpinUntilCo(const std::function<bool()>& ready, int timeout_ms)
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
 * 主线程有界等待收口闩锁：等待超时判失败，不无限阻塞测试进程。
 */
static void WaitLatchBounded(bbt::core::thread::CountDownLatch& latch, int timeout_ms)
{
    auto deadline = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(timeout_ms));
    while (!bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(deadline)) {
        if (latch.WaitTimeout(1) == 0)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    BOOST_ERROR("latch wait timed out");
}

BOOST_AUTO_TEST_SUITE(CoRWMutexTest)

BOOST_AUTO_TEST_CASE(t_start_scheduler)
{
    /**
     * 使用安全的协程栈大小，避免 ASAN 检测到的 stack-buffer-overflow
     * （默认 12KB 在某些协程操作中不足，导致间歇性 crash）
     *
     * 64KB
     */
    g_bbt_coroutine_config->m_cfg_stack_size = 64 * 1024;
    EnsureRuntime();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

/**
 * 测试读锁非阻塞
 */
BOOST_AUTO_TEST_CASE(t_rlock_block)
{
    bbt::core::thread::CountDownLatch l{1};
    bool succ = false;

    bbtco [&]() {
        auto cocond = bbtco_make_cocond();
        auto rwlock = bbtco_make_corwmutex();
        bbtco [=, &succ]() {
            rwlock->RLock();
            succ = true;
            cocond->Wait();
            rwlock->RUnLock();
        };

        bbtco_sleep(50);

        bbtco [=, &succ]() {
            bbtco_sleep(50);
            rwlock->RLock();
            BOOST_ASSERT(succ == true);
            cocond->NotifyAll();
            rwlock->RUnLock();
        };

        cocond->Wait();
        l.Down();
    };

    sleep(1);
    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_rlock_wlock_block)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main co") [&]() {
        int n = 0, m = 0;
        auto rwlock = bbtco_make_corwmutex();
        bbtco [&]() {
            rwlock->RLock();
            n++;
        };

        bbtco [&]() {
            m++;
            rwlock->WLock();
            n++;
        };

        bbtco_sleep(100);
        BOOST_ASSERT(n == m && n == 1);
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_rwlock_multi_co)
{
    std::atomic_bool running = true;
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_int nwrite{0};
    std::atomic_int nread{0};

    bbtco_desc("main") [&]() {
        std::atomic_bool in_writing{false};
        auto rwlock = bbtco_make_corwmutex();
        bbtco_desc("reader1") [&]() {
            while (running) {
                rwlock->RLock();
                BOOST_ASSERT(in_writing == false);
                nread.fetch_add(1, std::memory_order_relaxed);
                rwlock->RUnLock();
                bbtco_yield;
            }
        };

        bbtco_desc("reader2") [&]() {
            while (running) {
                rwlock->RLock();
                BOOST_ASSERT(in_writing == false);
                nread.fetch_add(1, std::memory_order_relaxed);
                rwlock->RUnLock();
            }
            bbtco_yield;
        };

        bbtco_desc("writer") [&]() {
            while (running) {
                rwlock->WLock();
                in_writing = true;
                in_writing = false;
                nwrite.fetch_add(1, std::memory_order_relaxed);
                rwlock->WUnLock();
            }
        };

        bbtco_sleep(1000);
        running = false;
        bbtco_sleep(100);
        l.Down();
    };

    l.Wait();
}

/**
 * 测试写优先：有 writer 排队时，新 reader 不得获取锁
 */
BOOST_AUTO_TEST_CASE(t_writer_priority)
{
    struct CaseState {
        bbt::coroutine::sync::CoRWMutex::SPtr rwlock{bbtco_make_corwmutex()};
        bbt::core::thread::CountDownLatch completed{1};
        std::atomic_bool reader_holding{false};
        std::atomic_bool writer_queued{false};
        std::atomic_bool reader_blocked{false};
        std::atomic_bool release_reader{false};
        std::atomic_bool writer_finished{false};
        std::atomic_bool reader_acquired_after_writer{false};
        std::atomic_int child_completed{0};
    };

    auto state = std::make_shared<CaseState>();
    std::weak_ptr<CaseState> weak_state{state};
    bbt::coroutine::sync::CoRWMutexTestAccess::SetWaitCallbacks(
        *state->rwlock,
        [weak_state] {
            if (auto shared_state = weak_state.lock())
                shared_state->writer_queued.store(true, std::memory_order_release);
        },
        [weak_state] {
            if (auto shared_state = weak_state.lock())
                shared_state->reader_blocked.store(true, std::memory_order_release);
        });

    bbtco [state]() {
        state->rwlock->RLock();
        state->reader_holding.store(true, std::memory_order_release);
        while (!state->release_reader.load(std::memory_order_acquire))
            bbtco_yield;
        state->rwlock->RUnLock();
        state->child_completed.fetch_add(1, std::memory_order_release);
    };

    bbtco [state]() {
        while (!state->reader_holding.load(std::memory_order_acquire))
            bbtco_yield;
        state->rwlock->WLock();
        state->writer_finished.store(true, std::memory_order_release);
        state->rwlock->WUnLock();
        state->child_completed.fetch_add(1, std::memory_order_release);
    };

    bbtco [state]() {
        while (!state->writer_queued.load(std::memory_order_acquire))
            bbtco_yield;
        state->rwlock->RLock();
        state->reader_acquired_after_writer.store(
            state->writer_finished.load(std::memory_order_acquire), std::memory_order_release);
        state->rwlock->RUnLock();
        state->child_completed.fetch_add(1, std::memory_order_release);
    };

    bbtco [state]() {
        while (!state->reader_blocked.load(std::memory_order_acquire))
            bbtco_yield;
        state->release_reader.store(true, std::memory_order_release);
        while (state->child_completed.load(std::memory_order_acquire) != 3)
            bbtco_yield;
        state->completed.Down();
    };

    state->completed.Wait();
    BOOST_CHECK(state->writer_finished.load(std::memory_order_acquire));
    BOOST_CHECK(state->reader_acquired_after_writer.load(std::memory_order_acquire));
}

/**
 * 测试 TryRLock 非阻塞获取
 */
BOOST_AUTO_TEST_CASE(t_try_rlock)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&]() {
        auto rwlock = bbtco_make_corwmutex();

        BOOST_ASSERT(rwlock->TryRLock() == 0);
        rwlock->RUnLock();

        rwlock->WLock();
        BOOST_ASSERT(rwlock->TryRLock() == -1);
        rwlock->WUnLock();

        l.Down();
    };

    l.Wait();
}

/**
 * 测试 TryWLock 非阻塞获取
 */
BOOST_AUTO_TEST_CASE(t_try_wlock)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&]() {
        auto rwlock = bbtco_make_corwmutex();

        BOOST_ASSERT(rwlock->TryWLock() == 0);
        rwlock->WUnLock();

        rwlock->RLock();
        BOOST_ASSERT(rwlock->TryWLock() == -1);
        rwlock->RUnLock();

        l.Down();
    };

    l.Wait();
}

/**
 * 测试 TryRLock(ms) 在 writer 持有时超时
 */
BOOST_AUTO_TEST_CASE(t_try_rlock_timeout)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&]() {
        auto rwlock = bbtco_make_corwmutex();

        rwlock->WLock();
        int ret = rwlock->TryRLock(50);
        BOOST_ASSERT(ret == 1);
        rwlock->WUnLock();

        BOOST_ASSERT(rwlock->TryRLock(100) == 0);
        rwlock->RUnLock();

        l.Down();
    };

    l.Wait();
}

/**
 * 测试 TryWLock(ms) 在 reader 持有时超时
 */
BOOST_AUTO_TEST_CASE(t_try_wlock_timeout)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&]() {
        auto rwlock = bbtco_make_corwmutex();

        rwlock->RLock();
        int ret = rwlock->TryWLock(50);
        BOOST_ASSERT(ret == 1);
        rwlock->RUnLock();

        BOOST_ASSERT(rwlock->TryWLock(100) == 0);
        rwlock->WUnLock();

        l.Down();
    };

    l.Wait();
}

/**
 * 测试 timeout 后继续正常使用锁
 */
BOOST_AUTO_TEST_CASE(t_timeout_recovery)
{
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_int n{0};

    bbtco_desc("main") [&]() {
        auto rwlock = bbtco_make_corwmutex();

        rwlock->WLock();

        bbtco_desc("timeout_reader") [rwlock]() {
            int ret = rwlock->TryRLock(50);
            BOOST_ASSERT(ret == 1);
        };

        bbtco_sleep(100);
        rwlock->WUnLock();

        bbtco_sleep(50);

        bbtco_desc("later_reader") [rwlock, &n]() {
            rwlock->RLock();
            n++;
            rwlock->RUnLock();
        };

        bbtco_sleep(100);
        BOOST_ASSERT(n == 1);
        l.Down();
    };

    l.Wait();
}

/**
 * =================== P0: TryWLock(ms) 超时后脏状态验证 ===================
 */

/**
 * TryWLock(ms) 超时后 WLock 仍可正常获取（m_has_wait_wlock 标志正确清除）
 */
BOOST_AUTO_TEST_CASE(t_try_wlock_timeout_then_wlock)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&]() {
        auto rwlock = bbtco_make_corwmutex();

        /**
         * 持有读锁
         */
        rwlock->RLock();

        /**
         * TryWLock 超时
         */
        int ret = rwlock->TryWLock(50);
        BOOST_ASSERT(ret == 1);

        /**
         * 释放读锁后，WLock 应该可以正常获取（m_has_wait_wlock 已清除）
         */
        rwlock->RUnLock();

        bbtco_sleep(20);
        rwlock->WLock();
        rwlock->WUnLock();

        BOOST_TEST(true);
        l.Down();
    };

    l.Wait();
}

/**
 * TryWLock(ms) 超时后 reader 不受影响（m_has_wait_wlock 标志正确清除）
 */
BOOST_AUTO_TEST_CASE(t_try_wlock_timeout_then_rlock)
{
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_int nread{0};

    bbtco_desc("main") [&]() {
        auto rwlock = bbtco_make_corwmutex();

        /**
         * 持有读锁
         */
        rwlock->RLock();

        /**
         * TryWLock 超时
         */
        int ret = rwlock->TryWLock(50);
        BOOST_ASSERT(ret == 1);

        rwlock->RUnLock();

        /**
         * 其他 reader 应该可以正常获取读锁（不被假 writer 阻塞）
         */
        bbtco [rwlock, &nread]() {
            rwlock->RLock();
            nread.fetch_add(1, std::memory_order_relaxed);
            rwlock->RUnLock();
        };

        bbtco_sleep(50);
        BOOST_TEST(nread.load(std::memory_order_acquire) == 1);
        l.Down();
    };

    l.Wait();
}

/**
 * 多次 TryWLock(ms) 超时后锁仍正常工作
 */
BOOST_AUTO_TEST_CASE(t_try_wlock_multiple_timeouts)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&]() {
        auto rwlock = bbtco_make_corwmutex();

        /**
         * 持有读锁，连续多次 TryWLock 超时
         */
        rwlock->RLock();

        for (int i = 0; i < 5; ++i) {
            int ret = rwlock->TryWLock(20);
            BOOST_ASSERT(ret == 1);
        }

        rwlock->RUnLock();

        /**
         * 释放后所有操作正常
         */
        BOOST_ASSERT(rwlock->TryWLock() == 0);
        rwlock->WUnLock();

        BOOST_ASSERT(rwlock->TryRLock() == 0);
        rwlock->RUnLock();

        BOOST_TEST(true);
        l.Down();
    };

    l.Wait();
}

/**
 * 增强多 reader / 多 writer 压力测试
 */
BOOST_AUTO_TEST_CASE(t_rwlock_multi_co_stress)
{
    std::atomic_bool running{true};
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_int nread{0};
    std::atomic_int nwrite{0};

    bbtco_desc("main") [&]() {
        std::atomic_int in_writing{0};
        std::atomic_int active_readers{0};
        auto rwlock = bbtco_make_corwmutex();

        for (int i = 0; i < 4; ++i) {
            bbtco [&]() {
                while (running) {
                    rwlock->RLock();
                    active_readers.fetch_add(1);
                    BOOST_ASSERT(in_writing == 0);
                    nread.fetch_add(1);
                    active_readers.fetch_sub(1);
                    rwlock->RUnLock();
                    bbtco_yield;
                }
            };
        }

        for (int i = 0; i < 3; ++i) {
            bbtco [&]() {
                while (running) {
                    rwlock->WLock();
                    int old = in_writing.fetch_add(1);
                    BOOST_ASSERT(old == 0);
                    BOOST_ASSERT(active_readers == 0);
                    nwrite.fetch_add(1);
                    in_writing.fetch_sub(1);
                    rwlock->WUnLock();
                    bbtco_yield;
                }
            };
        }

        bbtco_sleep(500);
        running = false;
        bbtco_sleep(100);
        BOOST_ASSERT(nread > 0);
        BOOST_ASSERT(nwrite > 0);
        l.Down();
    };

    l.Wait();
}

/**
 * WUnLock 一次性唤醒全部已挂起读者：两名读者在写者持锁期间完成挂起登记
 * （reader_blocked 回调即挂起阶段证明），写者释放后两人必须都获读锁。
 * 并发在场用单调到场计数证明：两人都在读锁内到场（arrived==2）后才允许
 * 离场，任何一方提前离场都会让对方永远看不到 2。
 * 抓错：唤醒发布只送达一个读者（NotifyAll 断链）、读者恢复后读到未发布的
 * 写者退出状态、挂起完成被延迟唤醒重复交付。
 * 断言全部在主线程做出。
 */
BOOST_AUTO_TEST_CASE(t_wunlock_wakes_all_blocked_readers)
{
    struct CaseState {
        bbt::coroutine::sync::CoRWMutex::SPtr rw{bbt::coroutine::sync::CoRWMutex::Create()};
        bbt::core::thread::CountDownLatch done{3};
        std::atomic_int blocked_readers{0};
        std::atomic_int writer_holding{0};
        std::atomic_int writer_released{0};
        std::atomic_int arrived{0};
        std::atomic_int concurrent_ok{0};
        std::atomic_int released_ok{0};
        std::atomic_int stage_writer_holding_ok{0};
        std::atomic_int stage_blocked_ok{0};
    };

    auto state = std::make_shared<CaseState>();
    std::weak_ptr<CaseState> weak_state{state};
    bbt::coroutine::sync::CoRWMutexTestAccess::SetWaitCallbacks(
        *state->rw,
        [weak_state] {},
        [weak_state] {
            if (auto shared_state = weak_state.lock())
                shared_state->blocked_readers.fetch_add(1, std::memory_order_release);
        });

    bbtco_desc("writer") [state]() {
        state->rw->WLock();
        state->writer_holding.store(1, std::memory_order_release);

        /**
         * 阶段门槛：两名读者都已完成挂起登记后再释放写锁
         */
        state->stage_blocked_ok.store(
            SpinUntilCo([&state] { return state->blocked_readers.load(std::memory_order_acquire) == 2; }, 3000) ? 1
                                                                                                                : 0);
        state->writer_released.store(1, std::memory_order_release);
        state->rw->WUnLock();
        state->done.Down();
    };

    bbtco_desc("conductor") [state]() {
        state->stage_writer_holding_ok.store(
            SpinUntilCo([&state] { return state->writer_holding.load(std::memory_order_acquire) == 1; }, 3000) ? 1 : 0);
        for (int i = 0; i < 2; ++i) {
            bbtco_desc("reader") [state]() {
                state->rw->RLock();
                state->released_ok.fetch_add(state->writer_released.load(std::memory_order_acquire) == 1 ? 1 : 0);

                /**
                 * 单调到场：两人都在读锁内到场后才允许离场
                 */
                state->arrived.fetch_add(1, std::memory_order_acq_rel);
                state->concurrent_ok.fetch_add(
                    SpinUntilCo([&state] { return state->arrived.load(std::memory_order_acquire) == 2; }, 3000) ? 1
                                                                                                                : 0);
                state->rw->RUnLock();
                state->done.Down();
            };
        }
    };

    WaitLatchBounded(state->done, 8000);

    BOOST_CHECK_EQUAL(state->stage_writer_holding_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->stage_blocked_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->arrived.load(), 2);
    BOOST_CHECK_EQUAL(state->concurrent_ok.load(), 2);
    BOOST_CHECK_EQUAL(state->released_ok.load(), 2);
}

/**
 * 带超时写者排队后超时退出，残留的死 waiter 不得吞掉解锁唤醒：
 * 排在其后的读者必须在读锁释放后获得读锁。
 * 抓错：_NotifyOne 停在已超时 waiter 上（读者挂死）、写者责任计数未结清
 * 导致写优先残留（读者永不放行）。
 * 释放标志在解锁前发布：读者获锁必然发生在该解锁之后，读取有效。
 * 断言全部在主线程做出。
 */
BOOST_AUTO_TEST_CASE(t_timedout_writer_then_blocked_reader_wakes)
{
    struct CaseState {
        bbt::coroutine::sync::CoRWMutex::SPtr rw{bbt::coroutine::sync::CoRWMutex::Create()};
        bbt::core::thread::CountDownLatch done{3};
        std::atomic_int writer_queued{0};
        std::atomic_int reader_blocked{0};
        std::atomic_int r0_holding{0};
        std::atomic_int w1_ret{-999};
        std::atomic_int w1_done{0};
        std::atomic_int r0_releasing{0};
        std::atomic_int r2_acquired{0};
        std::atomic_int stage_r0_ok{0};
        std::atomic_int stage_w1_queued_ok{0};
        std::atomic_int stage_r2_blocked_ok{0};
        std::atomic_int stage_w1_timeout_ok{0};
    };

    auto state = std::make_shared<CaseState>();
    std::weak_ptr<CaseState> weak_state{state};
    bbt::coroutine::sync::CoRWMutexTestAccess::SetWaitCallbacks(
        *state->rw,
        [weak_state] {
            if (auto shared_state = weak_state.lock())
                shared_state->writer_queued.fetch_add(1, std::memory_order_release);
        },
        [weak_state] {
            if (auto shared_state = weak_state.lock())
                shared_state->reader_blocked.fetch_add(1, std::memory_order_release);
        });

    bbtco_desc("conductor") [state]() {
        bbtco_desc("r0") [state]() {
            state->rw->RLock();
            state->r0_holding.store(1, std::memory_order_release);

            /**
             * 等 W1 按超时收口（写者责任已结清）再释放读锁；
             * 释放标志先于解锁发布，读者获锁后读取有效
             */
            state->stage_w1_timeout_ok.store(
                SpinUntilCo([&state] { return state->w1_done.load(std::memory_order_acquire) == 1; }, 3000) ? 1 : 0);
            state->r0_releasing.store(1, std::memory_order_release);
            state->rw->RUnLock();
            state->done.Down();
        };

        state->stage_r0_ok.store(
            SpinUntilCo([&state] { return state->r0_holding.load(std::memory_order_acquire) == 1; }, 3000) ? 1 : 0);

        bbtco_desc("w1") [state]() {
            state->w1_ret.store(state->rw->TryWLock(80), std::memory_order_release);
            state->w1_done.store(1, std::memory_order_release);
            state->done.Down();
        };

        /**
         * 阶段门槛 1：W1 已完成排队登记（写优先在此挡住后来读者）
         */
        state->stage_w1_queued_ok.store(
            SpinUntilCo([&state] { return state->writer_queued.load(std::memory_order_acquire) >= 1; }, 3000) ? 1 : 0);

        bbtco_desc("r2") [state]() {
            state->rw->RLock();
            state->r2_acquired.store(1, std::memory_order_release);
            state->rw->RUnLock();
            state->done.Down();
        };

        /**
         * 阶段门槛 2：R2 已被写优先挡入挂起，此后 R0 的释放必须绕过 W1 残留
         */
        state->stage_r2_blocked_ok.store(
            SpinUntilCo([&state] { return state->reader_blocked.load(std::memory_order_acquire) >= 1; }, 3000) ? 1 : 0);
    };

    WaitLatchBounded(state->done, 8000);

    BOOST_CHECK_EQUAL(state->stage_r0_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->stage_w1_queued_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->stage_r2_blocked_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->stage_w1_timeout_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->w1_ret.load(), 1);
    BOOST_CHECK_EQUAL(state->r0_releasing.load(), 1);
    BOOST_CHECK_EQUAL(state->r2_acquired.load(), 1);
}

BOOST_AUTO_TEST_CASE(t_stop_scheduler)
{
    /**
     * 等待所有 processer 线程完全退出，避免 Boost.Test 全局析构竞态
     * （未加等待时 Test_co_rwmutex 间歇性 segfault @0x2b8, ~20% 失败率）
     */
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

BOOST_AUTO_TEST_SUITE_END()
