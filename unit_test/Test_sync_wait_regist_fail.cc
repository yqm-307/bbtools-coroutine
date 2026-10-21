/**
 * @file Test_sync_wait_regist_fail.cc
 * @brief #368 + #369 回归：CoMutex / CoRWMutex 在等待事件登记失败时
 *        不重入已持有的内部 std::mutex、不因持锁重试阻断持锁方解锁，
 *        且不以旧唤醒掩码误报超时。
 *
 * 注入手段：Coroutine::_TestFailNextAwaitRegist() 置位后，下一次
 * _RegistAwaitEvent() 先把已创建的 await 事件 UnRegist 成 CANCELLED，
 * 再走真实 Regist() → -1 → YieldWithCallback 返回 -1。失败发生在事件
 * 注册阶段（RegistCustom 已成功返回），不是创建阶段。挂起回调（内含
 * _SysUnLock）因此未执行——修复前调用方会无条件重入 _SysLock，同一
 * 线程对非递归 std::mutex 二次加锁，属未定义行为（实测为死锁挂死）。
 *
 * #369 部分：CoMutex::TryLock(ms) 登记失败后不得把上一次等待的旧
 * POLL_EVENT_TIMEOUT/POLL_EVENT_CANCELLED 掩码误读为本轮超时。
 *
 * 确定性：协程内握手用 atomic 标志 + bbtco_yield 自旋屏障（真实条件，
 * 不猜 sleep 窗口）；CountDownLatch::Wait 基于 pthread_cond_wait 会
 * 阻塞 OS 线程，单 worker 下在协程内调用即死锁，故 latch 只在主线程
 * 汇合用。局部引用只在 done 归 0 后才离开作用域。ctest TIMEOUT 60
 * 兜底挂死。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <mutex>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
#include <bbt/coroutine/sync/CoRWMutex.hpp>
#include <bbt/coroutine/sync/CoLockGuard.hpp>
#include <cstdlib>
#include <new>
#include <system_error>
#include <stdexcept>
#include <pthread.h>
#include <cstdint>

/* 进程寿命模型（契约 §6）：runtime 只初始化一次，无 Stop/restart；
 * 静态配置在 Start 前一次写入，Start 后不回写（契约 §6.3：运行期不得
 * 并发修改静态配置字段）。 */
namespace
{
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, [](){
        /* 单 worker：协程内部顺序即确定；在 Start 前写入一次。 */
        g_bbt_coroutine_config->m_cfg_static_thread_num = 1;
        g_bbt_coroutine_config->m_cfg_stack_protect = false;
        bbt::coroutine::detail::Scheduler::GetInstance()->Start();
    });
}

/* 协程内因果屏障：等到 flag 为真，期间反复让权——等价"等另一协程
 * 推进到某点"，无 sleep 猜窗口。 */
inline void CoWaitFlag(const std::atomic_bool& flag)
{
    while (!flag.load(std::memory_order_acquire))
        bbtco_yield;
}

/* F3 单次分配失败注入：线程局部置位后下一次 operator new 抛 bad_alloc，
 * 一次性消费，不影响其它分配。与 reviewer 探针同型。 */
inline thread_local bool t_fail_next_alloc = false;
inline std::atomic<int>   t_alloc_hits{0};

/* F3-R2 序号注入：armed 期间对协程内每次 operator new 记序，命中 fail_on
 * 才抛 bad_alloc（一次性消费并解除 armed）；fail_on=0 仅计数不失败——
 * 用于先量出「等待准备」窗口的真实分配数，再逐点扫 1..total，保证覆盖
 * callback 装配与 Context 存储全部可分配点，而非只测反例出现的那一个。 */
inline thread_local bool t_alloc_armed = false;
inline thread_local int  t_alloc_seen = 0;
inline thread_local int  t_alloc_fail_on = 0;
/* F3-R2：armed 只计「属主协程」自身的 operator new——armed 横跨挂起点时，
 * 同 worker 上其它协程/调度侧推进的分配也会跑到本线程，若不加属主过滤，
 * 计量轮会把挂起期间的对端分配计入窗口、注入轮则可能在对端协程里引爆
 * bad_alloc 杀掉持锁者（实测死锁）。nullptr = 不过滤（兼容既有调用）。 */
inline thread_local const void* t_alloc_victim = nullptr;
/* TLS 协程指针在 Context::Resume 的挂起后回调期间仍指向 victim；
 * 只有执行地址位于协程栈才属于本测试的准备窗口。原生线程栈由
 * pthread 元数据取得，不依赖相邻栈地址、大小猜测或产品私有布局。 */
inline thread_local uintptr_t t_native_stack_begin = 0;
inline thread_local uintptr_t t_native_stack_end = 0;

/* writer_queued 回调写探针：与 Test_co_rwmutex.cc 中同名类按 ODR 等价
 * 定义，经 CoRWMutex 的 friend 声明访问私有回调槽。 */
}

namespace bbt::coroutine::sync
{
class CoRWMutexTestAccess
{
public:
    static void SetWaitCallbacks(CoRWMutex& rwlock,
                                 const std::function<void()>& writer_queued,
                                 const std::function<void()>& reader_blocked)
    {
        rwlock.m_writer_queued_callback = writer_queued;
        rwlock.m_reader_blocked_callback = reader_blocked;
    }
};
}

void* operator new(std::size_t n)
{
    char stack_marker;
    const auto stack_address = reinterpret_cast<uintptr_t>(&stack_marker);
    const bool native_stack = stack_address >= t_native_stack_begin &&
                              stack_address < t_native_stack_end;
    if (t_alloc_armed && !native_stack &&
        (t_alloc_victim == nullptr ||
         t_alloc_victim == g_bbt_tls_coroutine_co)) {
        const int ordinal = ++t_alloc_seen;
        if (t_alloc_fail_on != 0 && ordinal == t_alloc_fail_on) {
            t_alloc_armed = false;
            t_alloc_hits.fetch_add(1);
            throw std::bad_alloc();
        }
    }
    if (t_fail_next_alloc) {
        t_fail_next_alloc = false;
        t_alloc_hits.fetch_add(1);
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(SyncWaitRegistFailTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    EnsureRuntime();
    BOOST_REQUIRE(g_scheduler->IsInitialized());
}

/* ============ #368：锁不变量与进展 ============ */

/* 1. CoMutex 无限 Lock：另一协程持锁时，本协程登记失败不得死锁/重锁，
 *    且重试路径必须释放 m_mutex 让权——否则持锁方的 UnLock 被 m_mutex
 *    挡住、单 worker 也调度不到它，Lock 永不返回。
 *    时序（单 worker 确定）：victim 注入后 Lock → _WaitUnLock 登记失败
 *    → 释放 m_mutex 让权 → holder 被调度，can_unlock 已放行 → UnLock →
 *    victim 恢复重取 m_mutex 复查 m_status → 获锁返回。
 *    victim 先跑（它注入后立即 Lock），holder 的 UnLock 必然落在
 *    victim 首次失败让权之后——can_unlock 在 Lock() 前置位，不会比
 *    victim 的首次登记更早。 */
BOOST_AUTO_TEST_CASE(t_comutex_lock_regist_fail)
{
    auto mutex = sync::CoMutex::Create();
    std::atomic_bool holding{false};       /* holder→victim：已持锁 */
    std::atomic_bool can_unlock{false};    /* victim→holder：可解锁 */
    bbt::core::thread::CountDownLatch done{2};  /* 双方向主线程汇合 */
    std::atomic<int> lock_returned{0};

    bbtco [mutex, &holding, &can_unlock, &done]() {
        mutex->Lock();
        holding.store(true, std::memory_order_release);
        CoWaitFlag(can_unlock);          /* 协程让权自旋，不阻塞 worker */
        mutex->UnLock();
        done.Down();
    };

    bbtco [mutex, &holding, &can_unlock, &lock_returned, &done]() {
        CoWaitFlag(holding);             /* holder 已持锁：因果屏障 */
        Coroutine::_TestFailNextAwaitRegist();
        can_unlock.store(true, std::memory_order_release);
        mutex->Lock();                   /* 旧代码：重锁 m_mutex 挂死；
                                            持锁重试：m_mutex 挡 UnLock 挂死 */
        lock_returned.store(1);
        mutex->UnLock();
        done.Down();
    };

    /* done.Wait 在主线程 pthread 上等待，不占 worker；挂死由 ctest 兜底 */
    done.Wait();
    BOOST_CHECK_EQUAL(lock_returned.load(), 1);
}

/* 2. CoMutex::TryLock(-1) 竞争路径：RegistCustom 负超时建失败→ -1。
 *    修复前：-1 后仍 _SysLock 重锁。修复后：直接 -1，不重锁。 */
BOOST_AUTO_TEST_CASE(t_comutex_trylock_neg1_contended)
{
    auto mutex = sync::CoMutex::Create();
    std::atomic_bool holding{false}, can_unlock{false};
    bbt::core::thread::CountDownLatch done{2};
    std::atomic<int> ret{-99};

    bbtco [mutex, &holding, &can_unlock, &done]() {
        mutex->Lock();
        holding.store(true, std::memory_order_release);
        CoWaitFlag(can_unlock);
        mutex->UnLock();
        done.Down();
    };

    bbtco [mutex, &holding, &can_unlock, &ret, &done]() {
        CoWaitFlag(holding);
        ret.store(mutex->TryLock(-1));  /* 负期限：RegistCustom 拒建 */
        can_unlock.store(true, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(ret.load(), -1);
}

/* 3. CoMutex::TryLock(ms) 登记失败：返回 -1 且不挂死；之后锁对象
 *    仍可被真实获锁（重取 + 归还证据，不只"非阻塞调用返回任意值"）。 */
BOOST_AUTO_TEST_CASE(t_comutex_trylock_ms_regist_fail_and_reuse)
{
    auto mutex = sync::CoMutex::Create();
    std::atomic_bool holding{false}, can_unlock{false};
    bbt::core::thread::CountDownLatch done{2}, reuse{1};
    std::atomic<int> fail_ret{-99}, reacquired{0};

    bbtco [mutex, &holding, &can_unlock, &done]() {
        mutex->Lock();
        holding.store(true, std::memory_order_release);
        CoWaitFlag(can_unlock);
        mutex->UnLock();
        done.Down();
    };

    bbtco [mutex, &holding, &can_unlock, &fail_ret, &done]() {
        CoWaitFlag(holding);
        Coroutine::_TestFailNextAwaitRegist();
        fail_ret.store(mutex->TryLock(60000));  /* 登记失败 → -1，不重锁 */
        can_unlock.store(true, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(fail_ret.load(), -1);

    /* holder/victim 全部汇合后：另起协程真实重获锁，证明内部状态未污染 */
    bbtco [mutex, &reacquired, &reuse]() {
        if (mutex->TryLock() == 0) {
            reacquired.store(1);
            mutex->UnLock();
        }
        reuse.Down();
    };
    reuse.Wait();
    BOOST_CHECK_EQUAL(reacquired.load(), 1);
}

/* ============ #369：CoMutex TryLock(ms) 登记失败不误报超时 ============ */

/* 4. 固定旧掩码 + 注入登记失败 → 必须返回 -1 而非 1。
 *    单一 holder 持锁贯穿两次竞争窗口（同一 holder 因果握手）：victim
 *    先真实超时把 m_last_resume_event 固定为 TIMEOUT，再注入登记失败
 *    走同一路径。注入前显式断言 GetLastResumeEvent()&TIMEOUT 成立，
 *    且两次尝试之间无成功 yield/wait 介入（victim 顺序执行、无让权点，
 *    mask 断言与注入、第二次 TryLock 在同一运行段内）。 */
BOOST_AUTO_TEST_CASE(t_comutex_trylock_stale_mask)
{
    auto mutex = sync::CoMutex::Create();
    std::atomic_bool holding{false}, can_unlock{false};
    bbt::core::thread::CountDownLatch done{2};
    std::atomic<int> warm_ret{-99}, fail_ret{-99}, mask_ok{0};

    bbtco [mutex, &holding, &can_unlock, &done]() {
        mutex->Lock();
        holding.store(true, std::memory_order_release);
        CoWaitFlag(can_unlock);          /* 持锁直到 victim 两次尝试完成 */
        mutex->UnLock();
        done.Down();
    };

    bbtco [mutex, &holding, &can_unlock, &warm_ret, &fail_ret, &mask_ok, &done]() {
        CoWaitFlag(holding);
        /* 第一次：holder 持锁 → 真实超时 → 掩码固定为 TIMEOUT */
        warm_ret.store(mutex->TryLock(30));

        /* 掩码固定断言：上一次等待必须以 TIMEOUT 结束；自 warm_ret
         * 返回到此处没有 yield/wait，掩码未被其它等待覆写。 */
        mask_ok.store(
            (g_bbt_tls_coroutine_co->GetLastResumeEvent() & POLL_EVENT_TIMEOUT) ? 1 : 0);

        /* 第二次：holder 仍持锁（can_unlock 未放行），注入登记失败。
         * 必须是注册阶段失败（RegistCustom 已成功返回后
         * _RegistAwaitEvent 失败）——_TestFailNextAwaitRegist 正注入该点。 */
        Coroutine::_TestFailNextAwaitRegist();
        fail_ret.store(mutex->TryLock(60000));  /* 登记失败 → -1，不得误报 1 */
        can_unlock.store(true, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(warm_ret.load(), 1);   /* 真超时 → 1，掩码已固定 */
    BOOST_CHECK_EQUAL(mask_ok.load(), 1);    /* 注入前 GetLastResumeEvent 有 TIMEOUT */
    BOOST_CHECK_EQUAL(fail_ret.load(), -1);  /* 登记失败 → -1，不被旧掩码污染 */
}

/* ============ #368：CoRWMutex 锁不变量 ============ */

/* 5. CoRWMutex::RLock 无限等待 + 登记失败：不得重锁/挂死，返回 -1。 */
BOOST_AUTO_TEST_CASE(t_rwmutex_rlock_regist_fail)
{
    auto rw = sync::CoRWMutex::Create();
    std::atomic_bool holding{false}, can_unlock{false};
    bbt::core::thread::CountDownLatch done{2};
    std::atomic<int> ret{-99};

    bbtco [rw, &holding, &can_unlock, &done]() {
        rw->WLock();
        holding.store(true, std::memory_order_release);
        CoWaitFlag(can_unlock);
        rw->WUnLock();
        done.Down();
    };

    bbtco [rw, &holding, &can_unlock, &ret, &done]() {
        CoWaitFlag(holding);
        Coroutine::_TestFailNextAwaitRegist();
        ret.store(rw->RLock());   /* 旧代码：重锁 m_mutex 挂死 */
        can_unlock.store(true, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(ret.load(), -1);
}

/* 6. CoRWMutex::WLock 无限等待 + 登记失败：同上，且 m_has_wait_wlock
 *    不得残留为 true（否则后续读者被饿死）。reader 真实获锁验证。 */
BOOST_AUTO_TEST_CASE(t_rwmutex_wlock_regist_fail)
{
    auto rw = sync::CoRWMutex::Create();
    std::atomic_bool holding{false}, can_unlock{false};
    bbt::core::thread::CountDownLatch done{2}, done2{1};
    std::atomic<int> wret{-99}, rgot{0};

    bbtco [rw, &holding, &can_unlock, &done]() {
        rw->RLock();
        holding.store(true, std::memory_order_release);
        CoWaitFlag(can_unlock);
        rw->RUnLock();
        done.Down();
    };

    bbtco [rw, &holding, &can_unlock, &wret, &done]() {
        CoWaitFlag(holding);
        Coroutine::_TestFailNextAwaitRegist();
        wret.store(rw->WLock());  /* 旧代码：重锁挂死 */
        can_unlock.store(true, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(wret.load(), -1);

    /* m_has_wait_wlock 残留检测：reader 应真实拿到读锁并归还 */
    bbtco [rw, &rgot, &done2]() {
        if (rw->RLock() == 0) {
            rgot.store(1);
            rw->RUnLock();
        }
        done2.Down();
    };
    done2.Wait();
    BOOST_CHECK_EQUAL(rgot.load(), 1);
}

/* 7. CoRWMutex::TryRLock(ms) 登记失败：返回 -1，不重锁；
 *    之后对象仍可用（真实重获读锁）。 */
BOOST_AUTO_TEST_CASE(t_rwmutex_tryrlock_ms_regist_fail)
{
    auto rw = sync::CoRWMutex::Create();
    std::atomic_bool holding{false}, can_unlock{false};
    bbt::core::thread::CountDownLatch done{2}, done2{1};
    std::atomic<int> fail_ret{-99}, rgot{0};

    bbtco [rw, &holding, &can_unlock, &done]() {
        rw->WLock();
        holding.store(true, std::memory_order_release);
        CoWaitFlag(can_unlock);
        rw->WUnLock();
        done.Down();
    };

    bbtco [rw, &holding, &can_unlock, &fail_ret, &done]() {
        CoWaitFlag(holding);
        Coroutine::_TestFailNextAwaitRegist();
        fail_ret.store(rw->TryRLock(60000));
        can_unlock.store(true, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(fail_ret.load(), -1);

    bbtco [rw, &rgot, &done2]() {
        if (rw->TryRLock() == 0) {
            rgot.store(1);
            rw->RUnLock();
        }
        done2.Down();
    };
    done2.Wait();
    BOOST_CHECK_EQUAL(rgot.load(), 1);
}

/* 8. CoRWMutex::TryWLock(ms) 登记失败：返回 -1，m_has_wait_wlock
 *    不残留（在回调内置位，失败时回调未执行）。 */
BOOST_AUTO_TEST_CASE(t_rwmutex_trywlock_ms_regist_fail)
{
    auto rw = sync::CoRWMutex::Create();
    std::atomic_bool holding{false}, can_unlock{false};
    bbt::core::thread::CountDownLatch done{2}, done2{1};
    std::atomic<int> fail_ret{-99}, rgot{0};

    bbtco [rw, &holding, &can_unlock, &done]() {
        rw->RLock();
        holding.store(true, std::memory_order_release);
        CoWaitFlag(can_unlock);
        rw->RUnLock();
        done.Down();
    };

    bbtco [rw, &holding, &can_unlock, &fail_ret, &done]() {
        CoWaitFlag(holding);
        Coroutine::_TestFailNextAwaitRegist();
        fail_ret.store(rw->TryWLock(60000));
        can_unlock.store(true, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(fail_ret.load(), -1);

    bbtco [rw, &rgot, &done2]() {
        if (rw->RLock() == 0) {
            rgot.store(1);
            rw->RUnLock();
        }
        done2.Down();
    };
    done2.Wait();
    BOOST_CHECK_EQUAL(rgot.load(), 1);
}

/* 9. CoRWMutex::TryRLock(-1)/TryWLock(-1) 竞争：负期限立即返回 -1
 *    （_WaitRLockWithTimeout/_WaitWLockWithTimeout 的期限守卫），
 *    不挂死不重锁。 */
BOOST_AUTO_TEST_CASE(t_rwmutex_try_neg1_contended)
{
    auto rw = sync::CoRWMutex::Create();
    std::atomic_bool holding{false}, can_unlock{false};
    bbt::core::thread::CountDownLatch done{2};
    std::atomic<int> rret{-99}, wret{-99};

    bbtco [rw, &holding, &can_unlock, &done]() {
        rw->WLock();
        holding.store(true, std::memory_order_release);
        CoWaitFlag(can_unlock);
        rw->WUnLock();
        done.Down();
    };

    bbtco [rw, &holding, &can_unlock, &rret, &wret, &done]() {
        CoWaitFlag(holding);
        rret.store(rw->TryRLock(-1));
        wret.store(rw->TryWLock(-1));
        can_unlock.store(true, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(rret.load(), -1);
    BOOST_CHECK_EQUAL(wret.load(), -1);
}

/* 10. 无竞争边界：TryLock(-1) 在锁空闲时走非等待分支立即获锁（返回 0），
 *     期限守卫只拦截进入等待路径的负期限。 */
BOOST_AUTO_TEST_CASE(t_trylock_neg1_free)
{
    auto mutex = sync::CoMutex::Create();
    auto rw = sync::CoRWMutex::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<int> m_ret{-99}, r_ret{-99}, w_ret{-99};

    bbtco [mutex, rw, &m_ret, &r_ret, &w_ret, &done]() {
        m_ret.store(mutex->TryLock(-1));   /* 空闲：非等待分支，立即获锁 */
        if (m_ret.load() == 0) mutex->UnLock();
        r_ret.store(rw->TryRLock(-1));
        if (r_ret.load() == 0) rw->RUnLock();
        w_ret.store(rw->TryWLock(-1));
        if (w_ret.load() == 0) rw->WUnLock();
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(m_ret.load(), 0);
    BOOST_CHECK_EQUAL(r_ret.load(), 0);
    BOOST_CHECK_EQUAL(w_ret.load(), 0);
}

/* 11. 正常语义保留：成功获锁/真实超时/唤醒不受修复影响。
 *     子场景 B 用标志屏障保证 holder 持锁成立后 victim 才等待。 */
BOOST_AUTO_TEST_CASE(t_normal_paths_preserved)
{
    /* 子场景 A：空闲锁 TryLock()/TryLock(ms) 立即获锁 0。 */
    {
        auto free_mutex = sync::CoMutex::Create();
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<int> r0{-99}, rms{-99};
        bbtco [free_mutex, &r0, &rms, &done]() {
            r0.store(free_mutex->TryLock());
            free_mutex->UnLock();
            rms.store(free_mutex->TryLock(50));
            free_mutex->UnLock();
            done.Down();
        };
        done.Wait();
        BOOST_CHECK_EQUAL(r0.load(), 0);
        BOOST_CHECK_EQUAL(rms.load(), 0);
    }

    /* 子场景 B：竞争锁 TryLock(200) 真实超时 → 1。
     * holding 在 Lock() 返回之后置位；holder 持锁直到 victim 超时返回
     * （can_unlock 在 TryLock 返回后才置位），覆盖 200ms 超时窗口。 */
    {
        auto mutex = sync::CoMutex::Create();
        std::atomic_bool holding{false}, can_unlock{false};
        bbt::core::thread::CountDownLatch done{2};
        std::atomic<int> try_timeout{-99};

        bbtco [mutex, &holding, &can_unlock, &done]() {
            mutex->Lock();
            holding.store(true, std::memory_order_release);
            CoWaitFlag(can_unlock);
            mutex->UnLock();
            done.Down();
        };

        bbtco [mutex, &holding, &can_unlock, &try_timeout, &done]() {
            CoWaitFlag(holding);
            try_timeout.store(mutex->TryLock(200));
            can_unlock.store(true, std::memory_order_release);
            done.Down();
        };

        done.Wait();
        BOOST_CHECK_EQUAL(try_timeout.load(), 1);
    }
}

/* ============ R1-F1：RAII guard 的登记失败必须可观察 ============ */

/* 12. CoReadLock/CoWriteLock 构造与 lock() 在 RLock/WLock 返回 -1 时
 *     抛 std::system_error，不置 owns、不无声进临界区；失败 guard 析构
 *     不得归还别人的锁——持锁写者在场期间 TryRLock 仍应被拒（若失败
 *     * guard 析构误 RUnLock，读计数会减为 -1 并让读者在写锁下获锁）。 */
BOOST_AUTO_TEST_CASE(t_guard_regist_fail_observable)
{
    auto rw = sync::CoRWMutex::Create();
    std::atomic_bool holding{false}, can_unlock{false};
    bbt::core::thread::CountDownLatch done{2}, reuse{1};
    std::atomic<int> r_ctor{-1}, r_lock{-1}, w_ctor{-1}, w_lock{-1};
    std::atomic<int> read_blocked{0}, reacquired{0};

    bbtco [rw, &holding, &can_unlock, &done]() {
        rw->WLock();
        holding.store(true, std::memory_order_release);
        CoWaitFlag(can_unlock);          /* 持锁贯穿 victim 全部失败尝试 */
        rw->WUnLock();
        done.Down();
    };

    bbtco [rw, &holding, &can_unlock, &done,
          &r_ctor, &r_lock, &w_ctor, &w_lock, &read_blocked]() {
        CoWaitFlag(holding);

        Coroutine::_TestFailNextAwaitRegist();
        try { sync::CoReadLock g(rw); }
        catch (const std::system_error&) { r_ctor.store(1); }
        catch (...) { r_ctor.store(2); }
        /* 失败读 guard 析构后：写者仍持锁，读锁必须仍被拒绝（未误解锁） */
        read_blocked.store(rw->TryRLock() == -1 ? 1 : 0);

        Coroutine::_TestFailNextAwaitRegist();
        try {
            sync::CoReadLock g(rw, std::defer_lock);
            g.lock();
        } catch (const std::system_error&) { r_lock.store(1); }
        catch (...) { r_lock.store(2); }

        Coroutine::_TestFailNextAwaitRegist();
        try { sync::CoWriteLock g(rw); }
        catch (const std::system_error&) { w_ctor.store(1); }
        catch (...) { w_ctor.store(2); }

        Coroutine::_TestFailNextAwaitRegist();
        try {
            sync::CoWriteLock g(rw, std::defer_lock);
            g.lock();
        } catch (const std::system_error&) { w_lock.store(1); }
        catch (...) { w_lock.store(2); }

        can_unlock.store(true, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(r_ctor.load(), 1);
    BOOST_CHECK_EQUAL(r_lock.load(), 1);
    BOOST_CHECK_EQUAL(w_ctor.load(), 1);
    BOOST_CHECK_EQUAL(w_lock.load(), 1);
    BOOST_CHECK_EQUAL(read_blocked.load(), 1);

    /* 锁对象未污染：holder 归还后读锁可真实获取 */
    bbtco [rw, &reacquired, &reuse]() {
        if (rw->TryRLock() == 0) {
            reacquired.store(1);
            rw->RUnLock();
        }
        reuse.Down();
    };
    reuse.Wait();
    BOOST_CHECK_EQUAL(reacquired.load(), 1);
}

/* ============ R1-F2：先挂起→唤醒→被抢→再登记失败，不残留 writer 责任 ============ */

/* 13. writer A 首次挂起（回调置责任）→ 被读者解锁唤醒 → thief 抢先
 *     TryWLock → A 第二次等待登记失败返回 -1。不变量：
 *     (a) A 的责任已结清，但另一真实排队写者 C 仍存活时
 *         m_has_wait_wlock 不得被清——读者仍被拒；
 *     (b) C 完成获锁/解锁后，读者可正常获锁。
 *     单 worker 因果序由 writer_queued 回调计数（真实挂起，非标志猜测）
 *     与 atomic 屏障保证；注入仅在 C 已登记后武装，确保 A 的第二次
 *     _RegistAwaitEvent 唯一消费失败标志。 */
BOOST_AUTO_TEST_CASE(t_wlock_rewait_fail_keeps_live_writer)
{
    auto rw = sync::CoRWMutex::Create();
    std::atomic<int> writer_queued{0};   /* 真实挂起次数（回调内计数） */
    sync::CoRWMutexTestAccess::SetWaitCallbacks(*rw,
        [&writer_queued]{ writer_queued.fetch_add(1, std::memory_order_release); },
        {});

    std::atomic_bool holding{false}, a_done{false};
    bbt::core::thread::CountDownLatch done{3}, post{1};
    std::atomic<int> a_ret{-99}, steal_ret{-99}, read_during_c{-99}, final_read{-99};

    /* A、C 都先排队；单 worker 内解锁、抢锁和武装注入之间不让权，
     * 保证 A 首次恢复后的再次登记消费注入，C 仍是存活等待者。 */
    bbtco [rw, &writer_queued, &holding, &a_done,
          &steal_ret, &read_during_c, &done]() {
        rw->RLock();
        holding.store(true, std::memory_order_release);
        while (writer_queued.load(std::memory_order_acquire) < 2)
            bbtco_yield;
        rw->RUnLock();
        steal_ret.store(rw->TryWLock());
        Coroutine::_TestFailNextAwaitRegist();
        CoWaitFlag(a_done);
        rw->WUnLock();
        /* 锁已空闲，C 已就绪但单 worker 尚未恢复它。此时只有 C 的
         * 排队责任能拒绝读者，不能用仍持有写锁的状态代替本断言。 */
        read_during_c.store(rw->TryRLock());
        if (read_during_c.load() == 0)
            rw->RUnLock();
        done.Down();
    };

    /* writer A：挂起→被唤醒→被抢→再登记失败 → -1 */
    bbtco [rw, &holding, &a_done, &a_ret, &done]() {
        CoWaitFlag(holding);
        a_ret.store(rw->WLock());
        if (a_ret.load() == 0)
            rw->WUnLock();
        a_done.store(true, std::memory_order_release);
        done.Down();
    };

    /* C 必须排在 A 后面，回调计数 1 表明 A 已真实挂起。 */
    bbtco [rw, &writer_queued, &done]() {
        while (writer_queued.load(std::memory_order_acquire) < 1)
            bbtco_yield;
        if (rw->WLock() == 0)
            rw->WUnLock();
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(steal_ret.load(), 0);
    BOOST_CHECK_EQUAL(a_ret.load(), -1);
    BOOST_CHECK_EQUAL(read_during_c.load(), -1);   /* C 的写者责任仍在 */

    bbtco [rw, &final_read, &post]() {
        final_read.store(rw->TryRLock());
        if (final_read.load() == 0)
            rw->RUnLock();
        post.Down();
    };
    post.Wait();
    BOOST_CHECK_EQUAL(final_read.load(), 0);
}

/* ============ R1-F3：等待准备的一次分配失败必须释放内部锁 ============ */

/* 14. CoMutex::TryLock(ms) 持 m_mutex 准备等待时 RegistCustom 的
 *     CoPollEvent 构造抛 bad_alloc：旧代码锁残留、持锁方 UnLock 永久
 *     阻塞；修复后异常可观察（协程内捕获），holder 正常解锁，锁可复用。 */
BOOST_AUTO_TEST_CASE(t_comutex_wait_prep_bad_alloc_releases_lock)
{
    auto mutex = sync::CoMutex::Create();
    std::atomic_bool holding{false}, can_unlock{false};
    bbt::core::thread::CountDownLatch done{2}, reuse{1};
    std::atomic<int> caught{0}, holder_unlocked{0}, reacquired{0};

    bbtco [mutex, &holding, &can_unlock, &holder_unlocked, &done]() {
        mutex->Lock();
        holding.store(true, std::memory_order_release);
        CoWaitFlag(can_unlock);
        mutex->UnLock();                 /* 旧代码：残留 m_mutex 使此挂死 */
        holder_unlocked.store(1);
        done.Down();
    };

    bbtco [mutex, &holding, &can_unlock, &caught, &done]() {
        CoWaitFlag(holding);
        t_fail_next_alloc = true;        /* 下一次 make_shared<CoPollEvent> 必败 */
        try {
            mutex->TryLock(60000);
        } catch (const std::bad_alloc&) {
            caught.store(1);
        } catch (...) {
            caught.store(2);
        }
        t_fail_next_alloc = false;
        can_unlock.store(true, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(caught.load(), 1);
    BOOST_CHECK_GE(t_alloc_hits.load(), 1);
    BOOST_CHECK_EQUAL(holder_unlocked.load(), 1);

    bbtco [mutex, &reacquired, &reuse]() {
        if (mutex->TryLock() == 0) {
            reacquired.store(1);
            mutex->UnLock();
        }
        reuse.Down();
    };
    reuse.Wait();
    BOOST_CHECK_EQUAL(reacquired.load(), 1);
}

/* 15. CoRWMutex 同型：写者持锁时 RLock 的等待准备（CoWaiter 构造）抛
 *     bad_alloc——内部锁必须释放，写者解锁与后续读锁不受影响。 */
BOOST_AUTO_TEST_CASE(t_rwmutex_wait_prep_bad_alloc_releases_lock)
{
    auto rw = sync::CoRWMutex::Create();
    std::atomic_bool holding{false}, can_unlock{false};
    bbt::core::thread::CountDownLatch done{2}, reuse{1};
    std::atomic<int> caught{0}, writer_unlocked{0}, reacquired{0};

    bbtco [rw, &holding, &can_unlock, &writer_unlocked, &done]() {
        rw->WLock();
        holding.store(true, std::memory_order_release);
        CoWaitFlag(can_unlock);
        rw->WUnLock();
        writer_unlocked.store(1);
        done.Down();
    };

    bbtco [rw, &holding, &can_unlock, &caught, &done]() {
        CoWaitFlag(holding);
        t_fail_next_alloc = true;        /* 下一次 make_shared<CoWaiter> 必败 */
        try {
            rw->RLock();
        } catch (const std::bad_alloc&) {
            caught.store(1);
        } catch (...) {
            caught.store(2);
        }
        t_fail_next_alloc = false;
        can_unlock.store(true, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(caught.load(), 1);
    BOOST_CHECK_EQUAL(writer_unlocked.load(), 1);

    bbtco [rw, &reacquired, &reuse]() {
        if (rw->TryRLock() == 0) {
            reacquired.store(1);
            rw->RUnLock();
        }
        reuse.Down();
    };
    reuse.Wait();
    BOOST_CHECK_EQUAL(reacquired.load(), 1);
}

/* ============ R2-F3：callback 装配 / Context 存储分配失败的窗口回归 ============
 *
 * 与 #368-R1 已覆盖的「构造失败」不同，这里命中的是事件/队列准备成功之后
 * 的更深处故障点：协程内临时 std::function 包装、Coroutine 入参拷贝与
 * Context::m_onyield_callback 存储复制。R1 独立复审实证这些失败会留下
 * m_co_event/COND_WAIT、未清 await、stale 队列节点与 CO_SUSPEND 残留：
 *   - rw fail_on=3：第二次 TryRLock 得 -1（应真实超时 1）；
 *   - rw fail_on=4：Coroutine.cc CO_RUNNING 断言（CO_SUSPEND 残留）；
 *   - mutex fail_on=2：holder 解锁后第二 waiter 超时 1（stale 吞唤醒）。
 *
 * 方法：预热后计量同一 victim 协程栈上的准备分配，再逐序号单发注入。
 * 调度栈执行挂起后回调时 TLS 仍指向 victim，因此同时排除原生线程栈，
 * 不把注册域/诊断域的分配误当成尚未挂起的准备分配。扫描后核对最深
 * 故障命中、CO_RUNNING 恢复与后继等待进展；独立 scratch 原反例另核对
 * callback 包装和 Context 存储两个具体故障点。该计序验证只承诺本次
 * Linux/GCC/libstdc++ 环境，不以序号大小证明所有配置或持续 OOM 可恢复。 */

namespace
{
/* 与 F3 单次注入同型的窗口武装：armed 期间对本线程每次 operator new 记序，
 * fail_on!=0 命中序号抛 bad_alloc 一次性消费；fail_on=0 仅计数。 */
struct OrdinalGuard
{
    /* owner_coroutine_only=true：只记本协程的分配（默认）。构造必须在
     * 协程内，捕获 g_bbt_tls_coroutine_co 为属主。 */
    explicit OrdinalGuard(int fail_on)
    {
        if (t_native_stack_end == 0) {
            pthread_attr_t attributes;
            if (pthread_getattr_np(pthread_self(), &attributes) != 0)
                throw std::runtime_error("cannot inspect native worker stack");
            void* stack_base = nullptr;
            size_t stack_size = 0;
            const int result = pthread_attr_getstack(&attributes, &stack_base, &stack_size);
            pthread_attr_destroy(&attributes);
            if (result != 0)
                throw std::runtime_error("cannot resolve native worker stack bounds");
            t_native_stack_begin = reinterpret_cast<uintptr_t>(stack_base);
            t_native_stack_end = t_native_stack_begin + stack_size;
        }
        t_alloc_seen = 0;
        t_alloc_fail_on = fail_on;
        t_alloc_victim = g_bbt_tls_coroutine_co;
        t_alloc_armed = true;
    }
    ~OrdinalGuard() { Disarm(); }
    void Disarm()
    {
        t_alloc_armed = false;
        t_alloc_fail_on = 0;
        t_alloc_victim = nullptr;
    }
    int Seen() const { return t_alloc_seen; }
};

/* 协程内等 seq 到达 want：等价「等另一协程推进到某轮次」。 */
inline void CoWaitSeq(const std::atomic<int>& seq, int want)
{
    while (seq.load(std::memory_order_acquire) < want)
        bbtco_yield;
}

/* 协程内等 seq 到达 want 或 stop 置位：worker 退出条件。 */
inline bool CoWaitSeqOrStop(const std::atomic<int>& seq, int want,
                            const std::atomic<bool>& stop)
{
    while (seq.load(std::memory_order_acquire) < want) {
        if (stop.load(std::memory_order_acquire))
            return false;
        bbtco_yield;
    }
    return true;
}

/* 协程内等 seq 到达 want，带让出上限：给「对端在 victim 二次挂起期间
 * 必须主动推进」的兜底路径使用，保证单 worker 下有界收敛。 */
inline void CoWaitSeqBounded(const std::atomic<int>& seq, int want, int max_yields)
{
    for (int i = 0; i < max_yields && seq.load(std::memory_order_acquire) < want; ++i)
        bbtco_yield;
}
}  // namespace
/* 16-24：预热、计量、准备域序号扫描、后继进展验证。
 * owner 过滤加原生线程栈排除，使注入不会进入 Context::Resume 执行的
 * 挂起后注册回调；特别是严格诊断的 map 登记分配不在本测试保证内。
 * deep_oom 记录实际抛出的最深序号，>=2 排除仅测首个构造分配的情形；
 * 具体 callback/Context 故障点由父重编原审查探针的调用链对照补证。 */

namespace
{
/* 一次持锁轮：Lock → 等 go_seq==k → UnLock → 等 fin_seq==k。
 * lock_fn/unlock_fn 由调用方给出（Mutex/RWMutex R/W 三态）。 */
template <typename LockFn, typename UnlockFn>
inline void HolderRound(std::atomic<int>& held_seq,
                        const std::atomic<int>& go_seq,
                        const std::atomic<int>& fin_seq, int k,
                        const std::atomic<bool>& stop_all,
                        LockFn lock_fn, UnlockFn unlock_fn)
{
    lock_fn();
    held_seq.store(k, std::memory_order_release);
    if (!CoWaitSeqOrStop(go_seq, k, stop_all)) {
        unlock_fn();
        return;
    }
    unlock_fn();
    CoWaitSeqOrStop(fin_seq, k, stop_all);
}
}  // namespace

/* 16. CoMutex::Lock() 无限等待：全序号扫描 + 末位回滚验证。 */
BOOST_AUTO_TEST_CASE(t_comutex_lock_callback_oom_last_alloc)
{
    EnsureRuntime();
    auto mutex = sync::CoMutex::Create();
    bbt::core::thread::CountDownLatch done{2};
    std::atomic<int> held_seq{0}, go_seq{0}, fin_seq{0};
    std::atomic<bool> stop_all{false};
    std::atomic<int> total{0}, deep_oom{0}, oom_count{0};
    std::atomic<int> second_ret{-99}, status_bad{0}, req_bad{0};

    bbtco [mutex, &held_seq, &go_seq, &fin_seq, &stop_all, &total, &done]() {
        for (int k = 1; k <= 2; ++k)
            HolderRound(held_seq, go_seq, fin_seq, k, stop_all,
                        [mutex]{ mutex->Lock(); },
                        [mutex]{ mutex->UnLock(); });
        while (total.load(std::memory_order_acquire) == 0 &&
               !stop_all.load(std::memory_order_acquire))
            bbtco_yield;
        const int t = total.load(std::memory_order_acquire);
        for (int k = 3; k <= t + 3; ++k)
            HolderRound(held_seq, go_seq, fin_seq, k, stop_all,
                        [mutex]{ mutex->Lock(); },
                        [mutex]{ mutex->UnLock(); });
        done.Down();
    };

    bbtco [mutex, &held_seq, &go_seq, &fin_seq, &stop_all, &total,
          &deep_oom, &oom_count, &second_ret, &status_bad, &req_bad,
          &done]() {
        for (int k = 1; k <= 2; ++k) {
            if (!CoWaitSeqOrStop(held_seq, k, stop_all)) { done.Down(); return; }
            go_seq.store(k, std::memory_order_release);
            if (k == 1) {
                mutex->Lock();          /* 预热 */
                mutex->UnLock();
            } else {
                OrdinalGuard g(0);
                mutex->Lock();          /* 计量 */
                total.store(g.Seen(), std::memory_order_release);
                mutex->UnLock();
            }
            fin_seq.store(k, std::memory_order_release);
        }
        if (total.load() <= 0) {
            req_bad.fetch_add(1);
            done.Down();
            return;
        }
        for (int i = 1; i <= total.load(); ++i) {
            const int k = 2 + i;
            if (!CoWaitSeqOrStop(held_seq, k, stop_all)) { done.Down(); return; }
            go_seq.store(k, std::memory_order_release);
            int ret = -99, caught = 0;
            {
                OrdinalGuard g(i);
                try {
                    mutex->Lock();
                    ret = 0;
                } catch (const std::bad_alloc&) {
                    caught = 1;
                } catch (...) {
                    caught = 2;
                }
            }
            if (caught == 1) {
                oom_count.fetch_add(1);
                deep_oom.store(i, std::memory_order_release);
            } else if (ret == 0) {
                mutex->UnLock();    /* 非预期获锁，归还保持序 */
            }
            fin_seq.store(k, std::memory_order_release);
        }
        if (g_bbt_tls_coroutine_co->GetStatus() != CoroutineStatus::CO_RUNNING)
            status_bad.fetch_add(1);

        const int k_last = total.load() + 3;
        if (!CoWaitSeqOrStop(held_seq, k_last, stop_all)) { done.Down(); return; }
        go_seq.store(k_last, std::memory_order_release);
        second_ret.store(mutex->TryLock(1000));
        if (second_ret.load() == 0)
            mutex->UnLock();
        fin_seq.store(k_last, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_REQUIRE_EQUAL(req_bad.load(), 0);
    /* deep_oom>=2：至少一个序号越过首个分配（RegistCustom 构造），
     * 最深者即切换前的 Context 存储/装配点——不靠 threw>=1 掩饰。 */
    BOOST_REQUIRE_GE(deep_oom.load(), 2);
    BOOST_CHECK_EQUAL(status_bad.load(), 0);
    BOOST_CHECK_EQUAL(second_ret.load(), 0);
}

/* 17. CoMutex::TryLock(ms) 期限路径：同 16 扫描。 */
BOOST_AUTO_TEST_CASE(t_comutex_trylock_ms_callback_oom_last_alloc)
{
    EnsureRuntime();
    auto mutex = sync::CoMutex::Create();
    bbt::core::thread::CountDownLatch done{2};
    std::atomic<int> held_seq{0}, go_seq{0}, fin_seq{0};
    std::atomic<bool> stop_all{false};
    std::atomic<int> total{0}, deep_oom{0}, oom_count{0};
    std::atomic<int> second_ret{-99}, status_bad{0}, req_bad{0};

    bbtco [mutex, &held_seq, &go_seq, &fin_seq, &stop_all, &total, &done]() {
        for (int k = 1; k <= 2; ++k)
            HolderRound(held_seq, go_seq, fin_seq, k, stop_all,
                        [mutex]{ mutex->Lock(); },
                        [mutex]{ mutex->UnLock(); });
        while (total.load(std::memory_order_acquire) == 0 &&
               !stop_all.load(std::memory_order_acquire))
            bbtco_yield;
        const int t = total.load(std::memory_order_acquire);
        for (int k = 3; k <= t + 3; ++k)
            HolderRound(held_seq, go_seq, fin_seq, k, stop_all,
                        [mutex]{ mutex->Lock(); },
                        [mutex]{ mutex->UnLock(); });
        done.Down();
    };

    bbtco [mutex, &held_seq, &go_seq, &fin_seq, &stop_all, &total,
          &deep_oom, &oom_count, &second_ret, &status_bad, &req_bad,
          &done]() {
        for (int k = 1; k <= 2; ++k) {
            if (!CoWaitSeqOrStop(held_seq, k, stop_all)) { done.Down(); return; }
            go_seq.store(k, std::memory_order_release);
            int r;
            if (k == 1) {
                r = mutex->TryLock(60000);      /* 预热 */
            } else {
                OrdinalGuard g(0);
                r = mutex->TryLock(60000);      /* 计量 */
                total.store(g.Seen(), std::memory_order_release);
            }
            if (r != 0)
                req_bad.fetch_add(1);
            else
                mutex->UnLock();
            fin_seq.store(k, std::memory_order_release);
        }
        if (total.load() <= 0) {
            req_bad.fetch_add(1);
            done.Down();
            return;
        }
        for (int i = 1; i <= total.load(); ++i) {
            const int k = 2 + i;
            if (!CoWaitSeqOrStop(held_seq, k, stop_all)) { done.Down(); return; }
            go_seq.store(k, std::memory_order_release);
            int ret = -99, caught = 0;
            {
                OrdinalGuard g(i);
                try {
                    ret = mutex->TryLock(60000);
                } catch (const std::bad_alloc&) {
                    caught = 1;
                } catch (...) {
                    caught = 2;
                }
            }
            if (caught == 1) {
                oom_count.fetch_add(1);
                deep_oom.store(i, std::memory_order_release);
            } else if (ret == 0) {
                mutex->UnLock();
            }
            fin_seq.store(k, std::memory_order_release);
        }
        if (g_bbt_tls_coroutine_co->GetStatus() != CoroutineStatus::CO_RUNNING)
            status_bad.fetch_add(1);

        const int k_last = total.load() + 3;
        if (!CoWaitSeqOrStop(held_seq, k_last, stop_all)) { done.Down(); return; }
        go_seq.store(k_last, std::memory_order_release);
        second_ret.store(mutex->TryLock(1000));
        if (second_ret.load() == 0)
            mutex->UnLock();
        fin_seq.store(k_last, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_REQUIRE_EQUAL(req_bad.load(), 0);
    BOOST_REQUIRE_GE(deep_oom.load(), 2);
    BOOST_CHECK_EQUAL(status_bad.load(), 0);
    BOOST_CHECK_EQUAL(second_ret.load(), 0);
}

/* 18. CoRWMutex::RLock 无限等待（读者路径）：扫描 + 写者持锁下
 *     TryRLock(20) 真实超时回滚验证（R1 反例 second_ret=-1）。 */
BOOST_AUTO_TEST_CASE(t_rwmutex_rlock_callback_oom_last_alloc)
{
    EnsureRuntime();
    auto rw = sync::CoRWMutex::Create();
    bbt::core::thread::CountDownLatch done{2};
    std::atomic<int> held_seq{0}, go_seq{0}, fin_seq{0};
    std::atomic<int> verify_done{0};
    std::atomic<bool> stop_all{false};
    std::atomic<int> total{0}, deep_oom{0}, oom_count{0};
    std::atomic<int> second_ret{-99}, status_bad{0}, req_bad{0};

    /* k_last 轮：持写锁覆盖 victim 的 TryRLock(20) 全程——按 go_seq
     * 解锁会把本应超时的等待提前唤醒，故该轮等 verify_done。 */
    bbtco [rw, &held_seq, &go_seq, &fin_seq, &verify_done, &stop_all,
          &total, &done]() {
        for (int k = 1; k <= 2; ++k)
            HolderRound(held_seq, go_seq, fin_seq, k, stop_all,
                        [rw]{ rw->WLock(); },
                        [rw]{ rw->WUnLock(); });
        while (total.load(std::memory_order_acquire) == 0 &&
               !stop_all.load(std::memory_order_acquire))
            bbtco_yield;
        const int t = total.load(std::memory_order_acquire);
        for (int k = 3; k <= t + 2; ++k)
            HolderRound(held_seq, go_seq, fin_seq, k, stop_all,
                        [rw]{ rw->WLock(); },
                        [rw]{ rw->WUnLock(); });
        /* 验证轮：持写锁 → 等 verify_done → 解锁 */
        rw->WLock();
        held_seq.store(t + 3, std::memory_order_release);
        CoWaitSeqBounded(verify_done, 1, 400000);
        rw->WUnLock();
        done.Down();
    };

    bbtco [rw, &held_seq, &go_seq, &fin_seq, &verify_done, &stop_all,
          &total, &deep_oom, &oom_count, &second_ret, &status_bad,
          &req_bad, &done]() {
        for (int k = 1; k <= 2; ++k) {
            if (!CoWaitSeqOrStop(held_seq, k, stop_all)) { done.Down(); return; }
            go_seq.store(k, std::memory_order_release);
            int r;
            if (k == 1) {
                r = rw->RLock();
            } else {
                OrdinalGuard g(0);
                r = rw->RLock();
                total.store(g.Seen(), std::memory_order_release);
            }
            if (r != 0)
                req_bad.fetch_add(1);
            else
                rw->RUnLock();
            fin_seq.store(k, std::memory_order_release);
        }
        if (total.load() <= 0) {
            req_bad.fetch_add(1);
            done.Down();
            return;
        }
        for (int i = 1; i <= total.load(); ++i) {
            const int k = 2 + i;
            if (!CoWaitSeqOrStop(held_seq, k, stop_all)) { done.Down(); return; }
            go_seq.store(k, std::memory_order_release);
            int ret = -99, caught = 0;
            {
                OrdinalGuard g(i);
                try {
                    ret = rw->RLock();
                } catch (const std::bad_alloc&) {
                    caught = 1;
                } catch (...) {
                    caught = 2;
                }
            }
            if (caught == 1) {
                oom_count.fetch_add(1);
                deep_oom.store(i, std::memory_order_release);
            } else if (ret == 0) {
                rw->RUnLock();
            }
            fin_seq.store(k, std::memory_order_release);
        }
        if (g_bbt_tls_coroutine_co->GetStatus() != CoroutineStatus::CO_RUNNING)
            status_bad.fetch_add(1);

        const int k_last = total.load() + 3;
        if (!CoWaitSeqOrStop(held_seq, k_last, stop_all)) { done.Down(); return; }
        /* holder 持写锁中：真实超时 1（非 -1）证明等待位/await/
         * tombstone 全部回滚干净 */
        second_ret.store(rw->TryRLock(20));
        verify_done.store(1, std::memory_order_release);
        if (second_ret.load() == 0)
            rw->RUnLock();
        fin_seq.store(k_last, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_REQUIRE_EQUAL(req_bad.load(), 0);
    BOOST_REQUIRE_GE(deep_oom.load(), 2);
    BOOST_CHECK_EQUAL(status_bad.load(), 0);
    BOOST_CHECK_EQUAL(second_ret.load(), 1);   /* 真实超时，不是 -1 */
}

/* 19. CoRWMutex::TryRLock(ms) 期限路径：同 18 扫描。 */
BOOST_AUTO_TEST_CASE(t_rwmutex_tryrlock_ms_callback_oom_last_alloc)
{
    EnsureRuntime();
    auto rw = sync::CoRWMutex::Create();
    bbt::core::thread::CountDownLatch done{2};
    std::atomic<int> held_seq{0}, go_seq{0}, fin_seq{0};
    std::atomic<int> verify_done{0};
    std::atomic<bool> stop_all{false};
    std::atomic<int> total{0}, deep_oom{0}, oom_count{0};
    std::atomic<int> second_ret{-99}, status_bad{0}, req_bad{0};

    bbtco [rw, &held_seq, &go_seq, &fin_seq, &verify_done, &stop_all,
          &total, &done]() {
        for (int k = 1; k <= 2; ++k)
            HolderRound(held_seq, go_seq, fin_seq, k, stop_all,
                        [rw]{ rw->WLock(); },
                        [rw]{ rw->WUnLock(); });
        while (total.load(std::memory_order_acquire) == 0 &&
               !stop_all.load(std::memory_order_acquire))
            bbtco_yield;
        const int t = total.load(std::memory_order_acquire);
        for (int k = 3; k <= t + 2; ++k)
            HolderRound(held_seq, go_seq, fin_seq, k, stop_all,
                        [rw]{ rw->WLock(); },
                        [rw]{ rw->WUnLock(); });
        rw->WLock();
        held_seq.store(t + 3, std::memory_order_release);
        CoWaitSeqBounded(verify_done, 1, 400000);
        rw->WUnLock();
        done.Down();
    };

    bbtco [rw, &held_seq, &go_seq, &fin_seq, &verify_done, &stop_all,
          &total, &deep_oom, &oom_count, &second_ret, &status_bad,
          &req_bad, &done]() {
        for (int k = 1; k <= 2; ++k) {
            if (!CoWaitSeqOrStop(held_seq, k, stop_all)) { done.Down(); return; }
            go_seq.store(k, std::memory_order_release);
            int r;
            if (k == 1) {
                r = rw->TryRLock(60000);
            } else {
                OrdinalGuard g(0);
                r = rw->TryRLock(60000);
                total.store(g.Seen(), std::memory_order_release);
            }
            if (r != 0)
                req_bad.fetch_add(1);
            else
                rw->RUnLock();
            fin_seq.store(k, std::memory_order_release);
        }
        if (total.load() <= 0) {
            req_bad.fetch_add(1);
            done.Down();
            return;
        }
        for (int i = 1; i <= total.load(); ++i) {
            const int k = 2 + i;
            if (!CoWaitSeqOrStop(held_seq, k, stop_all)) { done.Down(); return; }
            go_seq.store(k, std::memory_order_release);
            int ret = -99, caught = 0;
            {
                OrdinalGuard g(i);
                try {
                    ret = rw->TryRLock(60000);
                } catch (const std::bad_alloc&) {
                    caught = 1;
                } catch (...) {
                    caught = 2;
                }
            }
            if (caught == 1) {
                oom_count.fetch_add(1);
                deep_oom.store(i, std::memory_order_release);
            } else if (ret == 0) {
                rw->RUnLock();
            }
            fin_seq.store(k, std::memory_order_release);
        }
        if (g_bbt_tls_coroutine_co->GetStatus() != CoroutineStatus::CO_RUNNING)
            status_bad.fetch_add(1);

        const int k_last = total.load() + 3;
        if (!CoWaitSeqOrStop(held_seq, k_last, stop_all)) { done.Down(); return; }
        second_ret.store(rw->TryRLock(20));
        verify_done.store(1, std::memory_order_release);
        if (second_ret.load() == 0)
            rw->RUnLock();
        fin_seq.store(k_last, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_REQUIRE_EQUAL(req_bad.load(), 0);
    BOOST_REQUIRE_GE(deep_oom.load(), 2);
    BOOST_CHECK_EQUAL(status_bad.load(), 0);
    BOOST_CHECK_EQUAL(second_ret.load(), 1);
}

/* 20. CoWaiter::WaitWithTimeoutAndCallback 直路：预热+计量+全序号扫描。
 *     每个命中装配序号的注入把异常抛出 API；扫完后
 *     WaitWithTimeout(30) 真超时 1、外部 Notify 完成等待 0——
 *     m_co_event/COND_WAIT 与 await 全清。 */
BOOST_AUTO_TEST_CASE(t_cowaiter_timeout_callback_oom_last_alloc)
{
    EnsureRuntime();
    auto waiter = sync::CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{2};
    std::atomic_bool go_notify{false};
    std::atomic<int> total{0}, deep_oom{0}, oom_count{0};
    std::atomic<int> timeout_ret{-99}, notify_ret{-99};
    std::atomic<int> status_bad{0}, req_bad{0};

    bbtco [waiter, &go_notify, &done]() {
        while (!go_notify.load(std::memory_order_acquire))
            bbtco_yield;
        for (int i = 0; i < 100000; ++i) {
            if (waiter->Notify() == 0)
                break;
            bbtco_yield;
        }
        done.Down();
    };

    bbtco [waiter, &go_notify, &total, &deep_oom, &oom_count,
          &timeout_ret, &notify_ret, &status_bad, &req_bad, &done]() {
        /* 预热+计量：30ms 定时器窗口不依赖外部动作 */
        {
            const int r = waiter->WaitWithTimeoutAndCallback(30, [] { return true; });
            if (r != 1)
                req_bad.fetch_add(1);
        }
        {
            OrdinalGuard g(0);
            const int r = waiter->WaitWithTimeoutAndCallback(30, [] { return true; });
            total.store(g.Seen(), std::memory_order_release);
            if (r != 1)
                req_bad.fetch_add(1);
        }
        if (total.load() <= 0)
            req_bad.fetch_add(1);
        for (int i = 1; i <= total.load(); ++i) {
            int caught = 0;
            {
                OrdinalGuard g(i);
                try {
                    waiter->WaitWithTimeoutAndCallback(30, [] { return true; });
                } catch (const std::bad_alloc&) {
                    caught = 1;
                } catch (...) {
                    caught = 2;
                }
            }
            if (caught == 1) {
                oom_count.fetch_add(1);
                deep_oom.store(i, std::memory_order_release);
            }
        }
        if (g_bbt_tls_coroutine_co->GetStatus() != CoroutineStatus::CO_RUNNING)
            status_bad.fetch_add(1);
        timeout_ret.store(waiter->WaitWithTimeout(30));
        go_notify.store(true, std::memory_order_release);
        notify_ret.store(waiter->WaitWithTimeout(2000));
        done.Down();
    };

    done.Wait();
    BOOST_REQUIRE_EQUAL(req_bad.load(), 0);
    BOOST_REQUIRE_GE(deep_oom.load(), 2);
    BOOST_CHECK_EQUAL(status_bad.load(), 0);
    BOOST_CHECK_EQUAL(timeout_ret.load(), 1);
    BOOST_CHECK_EQUAL(notify_ret.load(), 0);
}

/* 21. Coroutine::YieldWithCallback 直路：RegistCustom 在 armed 外预建
 *     30ms 定时事件，窗口内剩调用方临时包装 + Context 存储两处分配
 *     （参数为 const& 无入参拷贝）。全序号扫描 + await 归还/让出往返/
 *     Notify 等待验证。 */
BOOST_AUTO_TEST_CASE(t_coroutine_yield_with_callback_oom_last_alloc)
{
    EnsureRuntime();
    auto waiter = sync::CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{2};
    std::atomic_bool go_notify{false};
    std::atomic<int> total{0}, deep_oom{0}, oom_count{0};
    std::atomic<int> precheck{-99}, notify_ret{-99};
    std::atomic<int> status_bad{0}, progress_ok{0}, req_bad{0};

    bbtco [waiter, &go_notify, &done]() {
        while (!go_notify.load(std::memory_order_acquire))
            bbtco_yield;
        for (int i = 0; i < 100000; ++i) {
            if (waiter->Notify() == 0)
                break;
            bbtco_yield;
        }
        done.Down();
    };

    bbtco [waiter, &go_notify, &total, &deep_oom, &oom_count,
          &precheck, &notify_ret, &status_bad, &progress_ok, &req_bad,
          &done]() {
        auto* coroutine = g_bbt_tls_coroutine_co;
        /* payload 使闭包超出 std::function SBO（libstdc++ 约 16B）——
         * 否则 cb 构造与 m_onyield_callback 拷贝都不分配，窗口内无可
         * 注入的「装配/存储」分配点，deep_oom 会停在事件构造上。 */
        const std::string payload(64, 'x');
        for (int k = 0; k < 2; ++k) {
            auto ev0 = coroutine->RegistCustom(
                detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, 30);
            if (ev0 == nullptr) {
                req_bad.fetch_add(1);
                done.Down();
                return;
            }
            detail::CoroutineOnYieldCallback cb = [ev0, payload]() {
                return ev0->Regist() == 0 && !payload.empty();
            };
            if (k == 0) {
                const int r = coroutine->YieldWithCallback(cb);   /* 预热 */
                if (r != 0)
                    req_bad.fetch_add(1);
            } else {
                OrdinalGuard g(0);
                const int r = coroutine->YieldWithCallback(cb);   /* 计量 */
                total.store(g.Seen(), std::memory_order_release);
                if (r != 0)
                    req_bad.fetch_add(1);
            }
        }
        if (total.load() <= 0)
            req_bad.fetch_add(1);
        precheck.store(waiter->WaitWithTimeout(30));
        if (precheck.load() != 1)
            req_bad.fetch_add(1);

        for (int i = 1; i <= total.load(); ++i) {
            /* 1ms 定时事件：注入若命中 cb 构造，await 由 RegistCustom
             * 挂上但协程未让出——须以真实让出消费清账；1ms 保证消费
             * 路径不拖长用例。 */
            auto ev = coroutine->RegistCustom(
                detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, 1);
            if (ev == nullptr) {
                req_bad.fetch_add(1);
                break;
            }
            int caught = 0;
            {
                OrdinalGuard g(i);
                try {
                    detail::CoroutineOnYieldCallback cb = [ev, payload]() {
                        return ev->Regist() == 0 && !payload.empty();
                    };
                    coroutine->YieldWithCallback(cb);
                } catch (const std::bad_alloc&) {
                    caught = 1;
                } catch (...) {
                    caught = 2;
                }
            }
            if (caught == 1) {
                oom_count.fetch_add(1);
                deep_oom.store(i, std::memory_order_release);
            }
            /* 清账：捕获失败时 await 残留（RegistCustom 已挂），以一次
             * 真实让出消费——回调自身 Regist() 唤醒 ev（1ms 定时）；
             * await 已由回滚清空时这是一次无害空让出（ev 注册后无等
             * 待者，触发时由 poller 清账）。 */
            detail::CoroutineOnYieldCallback flush = [ev]() { return ev->Regist() == 0; };
            const int fr = coroutine->YieldWithCallback(flush);
            if (fr != 0)
                req_bad.fetch_add(1);
        }
        if (coroutine->GetStatus() != CoroutineStatus::CO_RUNNING)
            status_bad.fetch_add(1);
        coroutine->YieldAndPushGCoQueue();
        progress_ok.store(1);
        go_notify.store(true, std::memory_order_release);
        notify_ret.store(waiter->WaitWithTimeout(2000));
        done.Down();
    };

    done.Wait();
    BOOST_REQUIRE_EQUAL(req_bad.load(), 0);
    BOOST_REQUIRE_GE(deep_oom.load(), 2);
    BOOST_CHECK_EQUAL(status_bad.load(), 0);
    BOOST_CHECK_EQUAL(progress_ok.load(), 1);
    BOOST_CHECK_EQUAL(notify_ret.load(), 0);
}

/* ============ R2 修正轮新增：tombstone 多等待者与写者路径 ============ */

/* 22. CoMutex tombstone + 健康多等待者：B 先经「预热→计量→全序号扫
 *     描」自测最深可抛序号 deep_oom（每个扫描轮一次持锁对）；正式轮—
 *     —健康 A 已排队 → B 以 deep_oom 单发注入（tombstone 落队尾，不
 *     得误删队首 A——旧 pop() 恰好干这个）→ 健康 C 再排队 → holder
 *     解锁：_NotifyOne 消费 A 唤醒之；A 的 UnLock 再扫队列——跳过 B
 *     的 tombstone（及扫描期遗留 tombstone）唤醒 C。A/C 均真实获锁
 *     完成。 */
BOOST_AUTO_TEST_CASE(t_comutex_tombstone_keeps_healthy_waiters)
{
    EnsureRuntime();
    auto mutex = sync::CoMutex::Create();
    bbt::core::thread::CountDownLatch done{4};
    std::atomic<int> held_seq{0}, go_seq{0}, fin_seq{0};
    std::atomic<int> phase2{0}, a_queued{0}, b_done{0}, c_queued{0};
    std::atomic<int> total{0}, deep_oom{0};
    std::atomic<int> b_caught{0}, b_status_bad{0}, req_bad{0};
    std::atomic<int> a_ret{-99}, c_ret{-99};
    std::atomic<bool> stop_all{false};

    /* holder 段 1：B 的预热/计量/扫描每轮一次持锁（k=1..total+2）。
     * 段 2：Lock → phase2=2 → 待 C 排队 → UnLock 唤醒队首 A；A 的
     * UnLock 沿队列扫过 tombstone 唤醒 C。 */
    bbtco [mutex, &held_seq, &go_seq, &fin_seq, &phase2, &c_queued,
          &total, &stop_all, &done]() {
        for (int k = 1; k <= 2; ++k)
            HolderRound(held_seq, go_seq, fin_seq, k, stop_all,
                        [mutex]{ mutex->Lock(); },
                        [mutex]{ mutex->UnLock(); });
        while (total.load(std::memory_order_acquire) == 0 &&
               !stop_all.load(std::memory_order_acquire))
            bbtco_yield;
        const int t = total.load(std::memory_order_acquire);
        for (int k = 3; k <= t + 2; ++k)
            HolderRound(held_seq, go_seq, fin_seq, k, stop_all,
                        [mutex]{ mutex->Lock(); },
                        [mutex]{ mutex->UnLock(); });
        mutex->Lock();
        held_seq.store(t + 3, std::memory_order_release);
        phase2.store(2, std::memory_order_release);
        while (c_queued.load(std::memory_order_acquire) == 0)
            bbtco_yield;
        mutex->UnLock();                /* 唤醒队首 A */
        done.Down();
    };

    /* A：phase2 后置 flag 挂起（队首的活等待者——前面可能有扫描期
     * 遗留 tombstone，A 仍是第一个存活节点） */
    bbtco [mutex, &phase2, &a_queued, &a_ret, &done]() {
        while (phase2.load(std::memory_order_acquire) < 2)
            bbtco_yield;
        a_queued.store(1, std::memory_order_release);
        mutex->Lock();
        a_ret.store(0);
        mutex->UnLock();                /* 跳过 tombstone 唤醒 C */
        done.Down();
    };

    /* B：段 1 预热+计量+扫描测得 deep_oom；段 2 在 A 之后注入 */
    bbtco [mutex, &held_seq, &go_seq, &fin_seq, &phase2, &a_queued,
          &b_done, &total, &deep_oom, &b_caught, &b_status_bad, &req_bad,
          &stop_all, &done]() {
        for (int k = 1; k <= 2; ++k) {
            if (!CoWaitSeqOrStop(held_seq, k, stop_all)) { done.Down(); return; }
            go_seq.store(k, std::memory_order_release);
            if (k == 1) {
                mutex->Lock();
                mutex->UnLock();
            } else {
                OrdinalGuard g(0);
                mutex->Lock();
                total.store(g.Seen(), std::memory_order_release);
                mutex->UnLock();
            }
            fin_seq.store(k, std::memory_order_release);
        }
        if (total.load() <= 0) {
            req_bad.fetch_add(1);
            done.Down();
            return;
        }
        for (int i = 1; i <= total.load(); ++i) {
            const int k = 2 + i;
            if (!CoWaitSeqOrStop(held_seq, k, stop_all)) { done.Down(); return; }
            go_seq.store(k, std::memory_order_release);
            int ret = -99, caught = 0;
            {
                OrdinalGuard g(i);
                try {
                    mutex->Lock();
                    ret = 0;
                } catch (const std::bad_alloc&) {
                    caught = 1;
                } catch (...) {
                    caught = 2;
                }
            }
            if (caught == 1)
                deep_oom.store(i, std::memory_order_release);
            else if (ret == 0)
                mutex->UnLock();
            fin_seq.store(k, std::memory_order_release);
        }
        if (deep_oom.load() < 2) {
            req_bad.fetch_add(1);
            done.Down();
            return;
        }

        /* 段 2：holder 再持锁 + A 已排队 → B 以最深序号注入，
         * tombstone 落队尾 */
        while (phase2.load(std::memory_order_acquire) < 2)
            bbtco_yield;
        while (a_queued.load(std::memory_order_acquire) == 0)
            bbtco_yield;
        {
            OrdinalGuard g(deep_oom.load());
            try {
                mutex->Lock();
            } catch (const std::bad_alloc&) {
                b_caught.store(1);
            } catch (...) {
                b_caught.store(2);
            }
        }
        if (g_bbt_tls_coroutine_co->GetStatus() != CoroutineStatus::CO_RUNNING)
            b_status_bad.fetch_add(1);
        b_done.store(1, std::memory_order_release);
        done.Down();
    };

    /* C：B 失败后排队（tombstone 之后）→ 被 A 的 UnLock 唤醒 */
    bbtco [mutex, &b_done, &c_queued, &c_ret, &done]() {
        while (b_done.load(std::memory_order_acquire) == 0)
            bbtco_yield;
        c_queued.store(1, std::memory_order_release);
        mutex->Lock();
        c_ret.store(0);
        mutex->UnLock();
        done.Down();
    };

    done.Wait();
    BOOST_REQUIRE_EQUAL(req_bad.load(), 0);
    BOOST_REQUIRE_EQUAL(b_caught.load(), 1);
    BOOST_CHECK_EQUAL(b_status_bad.load(), 0);
    BOOST_CHECK_EQUAL(a_ret.load(), 0);
    BOOST_CHECK_EQUAL(c_ret.load(), 0);
}

/* 23. CoRWMutex 写者路径 tombstone + 多等待者：同 22 结构——读者
 *     holder 持锁，B 自测 deep_oom 后在写者 A 之后末位注入（写者
 *     tombstone 落队尾），写者 C 再排队；读者 RUnLock → _NotifyOne
 *     唤醒 A；A 的 WUnLock 扫写者队列跳过 tombstone 唤醒 C。
 *     写者优先由消费顺序维持，不在持锁路径反复扫描。 */
BOOST_AUTO_TEST_CASE(t_rwmutex_writer_tombstone_and_priority)
{
    EnsureRuntime();
    auto rw = sync::CoRWMutex::Create();
    bbt::core::thread::CountDownLatch done{4};
    std::atomic<int> held_seq{0}, go_seq{0}, fin_seq{0};
    std::atomic<int> phase2{0}, a_queued{0}, b_done{0}, c_queued{0};
    std::atomic<int> total{0}, deep_oom{0};
    std::atomic<int> b_caught{0}, b_status_bad{0}, req_bad{0};
    std::atomic<int> a_ret{-99}, c_ret{-99};
    std::atomic<bool> stop_all{false};

    bbtco [rw, &held_seq, &go_seq, &fin_seq, &phase2, &c_queued,
          &total, &stop_all, &done]() {
        for (int k = 1; k <= 2; ++k)
            HolderRound(held_seq, go_seq, fin_seq, k, stop_all,
                        [rw]{ rw->RLock(); },
                        [rw]{ rw->RUnLock(); });
        while (total.load(std::memory_order_acquire) == 0 &&
               !stop_all.load(std::memory_order_acquire))
            bbtco_yield;
        const int t = total.load(std::memory_order_acquire);
        for (int k = 3; k <= t + 2; ++k)
            HolderRound(held_seq, go_seq, fin_seq, k, stop_all,
                        [rw]{ rw->RLock(); },
                        [rw]{ rw->RUnLock(); });
        rw->RLock();
        held_seq.store(t + 3, std::memory_order_release);
        phase2.store(2, std::memory_order_release);
        while (c_queued.load(std::memory_order_acquire) == 0)
            bbtco_yield;
        rw->RUnLock();                  /* _NotifyOne 唤醒写者队首 A */
        done.Down();
    };

    bbtco [rw, &phase2, &a_queued, &a_ret, &done]() {
        while (phase2.load(std::memory_order_acquire) < 2)
            bbtco_yield;
        a_queued.store(1, std::memory_order_release);
        rw->WLock();
        a_ret.store(0);
        rw->WUnLock();                  /* 跳过 tombstone 唤醒 C */
        done.Down();
    };

    bbtco [rw, &held_seq, &go_seq, &fin_seq, &phase2, &a_queued,
          &b_done, &total, &deep_oom, &b_caught, &b_status_bad, &req_bad,
          &stop_all, &done]() {
        for (int k = 1; k <= 2; ++k) {
            if (!CoWaitSeqOrStop(held_seq, k, stop_all)) { done.Down(); return; }
            go_seq.store(k, std::memory_order_release);
            int r;
            if (k == 1) {
                r = rw->WLock();
            } else {
                OrdinalGuard g(0);
                r = rw->WLock();
                total.store(g.Seen(), std::memory_order_release);
            }
            if (r != 0)
                req_bad.fetch_add(1);
            else
                rw->WUnLock();
            fin_seq.store(k, std::memory_order_release);
        }
        if (total.load() <= 0) {
            req_bad.fetch_add(1);
            done.Down();
            return;
        }
        for (int i = 1; i <= total.load(); ++i) {
            const int k = 2 + i;
            if (!CoWaitSeqOrStop(held_seq, k, stop_all)) { done.Down(); return; }
            go_seq.store(k, std::memory_order_release);
            int ret = -99, caught = 0;
            {
                OrdinalGuard g(i);
                try {
                    ret = rw->WLock();
                } catch (const std::bad_alloc&) {
                    caught = 1;
                } catch (...) {
                    caught = 2;
                }
            }
            if (caught == 1)
                deep_oom.store(i, std::memory_order_release);
            else if (ret == 0)
                rw->WUnLock();
            fin_seq.store(k, std::memory_order_release);
        }
        if (deep_oom.load() < 2) {
            req_bad.fetch_add(1);
            done.Down();
            return;
        }
        while (phase2.load(std::memory_order_acquire) < 2)
            bbtco_yield;
        while (a_queued.load(std::memory_order_acquire) == 0)
            bbtco_yield;
        {
            OrdinalGuard g(deep_oom.load());
            try {
                rw->WLock();
            } catch (const std::bad_alloc&) {
                b_caught.store(1);
            } catch (...) {
                b_caught.store(2);
            }
        }
        if (g_bbt_tls_coroutine_co->GetStatus() != CoroutineStatus::CO_RUNNING)
            b_status_bad.fetch_add(1);
        b_done.store(1, std::memory_order_release);
        done.Down();
    };

    bbtco [rw, &b_done, &c_queued, &c_ret, &done]() {
        while (b_done.load(std::memory_order_acquire) == 0)
            bbtco_yield;
        c_queued.store(1, std::memory_order_release);
        rw->WLock();
        c_ret.store(0);
        rw->WUnLock();
        done.Down();
    };

    done.Wait();
    BOOST_REQUIRE_EQUAL(req_bad.load(), 0);
    BOOST_REQUIRE_EQUAL(b_caught.load(), 1);
    BOOST_CHECK_EQUAL(b_status_bad.load(), 0);
    BOOST_CHECK_EQUAL(a_ret.load(), 0);
    BOOST_CHECK_EQUAL(c_ret.load(), 0);
}

/* 24. CoRWMutex::TryWLock(ms) 写者期限路径：同族扫描 + 写者责任无
 *     残留验证——B 失败时挂起回调未执行、m_wait_wlock_num 不 +1；
 *     回滚后锁空闲侧读锁即获（无写者责任残留），真实读锁下写者仍
 *     被拒（计数协议未失真）。 */
BOOST_AUTO_TEST_CASE(t_rwmutex_trywlock_ms_callback_oom_last_alloc)
{
    EnsureRuntime();
    auto rw = sync::CoRWMutex::Create();
    bbt::core::thread::CountDownLatch done{2};
    std::atomic<int> held_seq{0}, go_seq{0}, fin_seq{0};
    std::atomic<int> verify_done{0};
    std::atomic<bool> stop_all{false};
    std::atomic<int> total{0}, deep_oom{0}, oom_count{0};
    std::atomic<int> read_after{-99}, read_blocked{-99};
    std::atomic<int> status_bad{0}, req_bad{0};

    /* k_last 轮持读锁覆盖验证：victim 在其间做 TryRLock()/TryWLock()
     * 快路径检查（无挂起），holder 等 verify_done 后解锁。 */
    bbtco [rw, &held_seq, &go_seq, &fin_seq, &verify_done, &stop_all,
          &total, &done]() {
        for (int k = 1; k <= 2; ++k)
            HolderRound(held_seq, go_seq, fin_seq, k, stop_all,
                        [rw]{ rw->RLock(); },
                        [rw]{ rw->RUnLock(); });
        while (total.load(std::memory_order_acquire) == 0 &&
               !stop_all.load(std::memory_order_acquire))
            bbtco_yield;
        const int t = total.load(std::memory_order_acquire);
        for (int k = 3; k <= t + 2; ++k)
            HolderRound(held_seq, go_seq, fin_seq, k, stop_all,
                        [rw]{ rw->RLock(); },
                        [rw]{ rw->RUnLock(); });
        rw->RLock();
        held_seq.store(t + 3, std::memory_order_release);
        CoWaitSeqBounded(verify_done, 1, 400000);
        rw->RUnLock();
        done.Down();
    };

    bbtco [rw, &held_seq, &go_seq, &fin_seq, &verify_done, &stop_all,
          &total, &deep_oom, &oom_count, &status_bad, &read_after,
          &read_blocked, &req_bad, &done]() {
        for (int k = 1; k <= 2; ++k) {
            if (!CoWaitSeqOrStop(held_seq, k, stop_all)) { done.Down(); return; }
            go_seq.store(k, std::memory_order_release);
            int r;
            if (k == 1) {
                r = rw->TryWLock(60000);
            } else {
                OrdinalGuard g(0);
                r = rw->TryWLock(60000);
                total.store(g.Seen(), std::memory_order_release);
            }
            if (r != 0)
                req_bad.fetch_add(1);
            else
                rw->WUnLock();
            fin_seq.store(k, std::memory_order_release);
        }
        if (total.load() <= 0) {
            req_bad.fetch_add(1);
            done.Down();
            return;
        }
        for (int i = 1; i <= total.load(); ++i) {
            const int k = 2 + i;
            if (!CoWaitSeqOrStop(held_seq, k, stop_all)) { done.Down(); return; }
            go_seq.store(k, std::memory_order_release);
            int ret = -99, caught = 0;
            {
                OrdinalGuard g(i);
                try {
                    ret = rw->TryWLock(60000);
                } catch (const std::bad_alloc&) {
                    caught = 1;
                } catch (...) {
                    caught = 2;
                }
            }
            if (caught == 1) {
                oom_count.fetch_add(1);
                deep_oom.store(i, std::memory_order_release);
            } else if (ret == 0) {
                rw->WUnLock();
            }
            fin_seq.store(k, std::memory_order_release);
        }
        if (g_bbt_tls_coroutine_co->GetStatus() != CoroutineStatus::CO_RUNNING)
            status_bad.fetch_add(1);

        const int k_last = total.load() + 3;
        if (!CoWaitSeqOrStop(held_seq, k_last, stop_all)) { done.Down(); return; }
        /* holder 持读锁中：读者 TryRLock 即获——前提 m_wait_wlock_num==0
         * 且 m_has_wait_wlock 未误置（写者责任无残留）；再自加持读锁
         * 下 TryWLock 仍被拒——计数协议区分存活等待者未失真。 */
        read_after.store(rw->TryRLock());
        if (read_after.load() == 0)
            rw->RUnLock();
        rw->RLock();
        read_blocked.store(rw->TryWLock());
        rw->RUnLock();
        verify_done.store(1, std::memory_order_release);
        fin_seq.store(k_last, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_REQUIRE_EQUAL(req_bad.load(), 0);
    BOOST_REQUIRE_GE(deep_oom.load(), 2);
    BOOST_CHECK_EQUAL(status_bad.load(), 0);
    BOOST_CHECK_EQUAL(read_after.load(), 0);
    BOOST_CHECK_EQUAL(read_blocked.load(), -1);
}

BOOST_AUTO_TEST_SUITE_END()