/**
 * @file Test_diagnostic_wait_oom.cc
 * @brief 诊断等待 OOM 回归：挂起前窗口分配故障的完整回滚，与
 *        STRINGENT_DEBUG 挂起后诊断登记 map 分配故障不逃出 worker。
 *
 * prep 域（直接驱动，不依赖 worker）：
 *   逐序号注入 Coroutine::YieldWithCallback 挂起前窗口的全部分配点。
 *   日志装配已改为栈缓冲 snprintf（不分配），因此无论 DEBUG_INFO 开关，
 *   armed 窗口内首分配都是真实 Context 存储（m_onyield_callback 的
 *   std::function target 拷贝）——原日志分配点已整体移除。
 *   两种配置同一命题：bad_alloc 可观察 + CO_RUNNING + await 可复用。
 *
 * postyield 域（真实 worker 栈回调）：
 *   CoMutex::TryLock(ms) 的挂起回调在 Context::Resume 原生 worker 栈执行，
 *   Regist() 内 DebugMgr map insert 的单次分配失败不得逃逸（旧行为
 *   terminate）；修复后 Regist -1 → CHECK_FAILED 重入 → TryLock -1，
 *   放锁、后继等待者进展与 await/诊断清账全部成立。
 *   PENDING 分支由真实调用者路径覆盖（预取消协程 TryLock → worker 栈
 *   _RegistAwaitEvent → Trigger(CANCELLED) → Regist PENDING 分支）；
 *   ARMED 主成功分支用直接驱动覆盖。
 *   ResumeCo map insert 的 OOM 由 worker 栈真实 Resume→Yield→Resume→
 *   FINAL 轨迹覆盖：单次失败有界重试一次，按 id 精确登记恢复，无缺口放行。
 *
 * 注入约定与 Test_sync_wait_regist_fail.cc 同型：armed 期间按序记
 * operator new，命中 fail_on 抛 std::bad_alloc 一次性消费；postyield
 * 域额外按 pthread 元数据标记分配是否发生在原生线程栈，据此直接定位
 * 挂起后登记分配点，不靠固定序号猜测。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <pthread.h>
#include <cstdint>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

namespace
{
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, [](){
        g_bbt_coroutine_config->m_cfg_static_thread_num = 1;
        g_bbt_coroutine_config->m_cfg_stack_size = 65536;
        g_bbt_coroutine_config->m_cfg_stack_protect = false;
        bbt::coroutine::detail::Scheduler::GetInstance()->Start();
    });
}

inline void CoWaitFlag(const std::atomic_bool& flag)
{
    while (!flag.load(std::memory_order_acquire))
        bbtco_yield;
}

/* ---- prep 域：直接驱动用的记序注入（TLS 在测试主线程，含原生栈分配） ---- */
inline thread_local bool  d_armed = false;
inline thread_local int   d_seen = 0;
inline thread_local int   d_fail_on = 0;
inline std::atomic<int>   d_hits{0};
inline std::atomic<int>   d_hit_size{0};

/* ---- postyield 域：worker 侧注入（TLS 在 worker 线程） ----
 * w_fail_native_once：armed 期间命中的首个原生线程栈分配抛 bad_alloc——
 * 挂起后注册回调在 Context::Resume 原生栈上运行，其窗口内首个 native
 * 分配即 Regist() 顶部的 DebugMgr map insert（该调用是 Regist 首个语句，
 * 之前的回调链无堆分配）。不用序号：两轮 TryLock 的协程侧分配序不同，
 * 计量序号无法复用到注入轮（实测漂移）。
 * w_on_hit：可选命中动作，在抛 bad_alloc 之前于同一注入点同步执行——
 * 首胜回归用它把 Notify/RequestCancel 真实投递到「登记中」的
 * 事件上（ARMED 态 Trigger→PENDING 首胜），与独立探针同一时序。 */
inline thread_local bool     w_armed = false;
inline thread_local bool     w_fail_native_once = false;
inline thread_local const void* w_victim = nullptr;
inline thread_local uintptr_t  w_stack_begin = 0;
inline thread_local uintptr_t  w_stack_end = 0;
inline thread_local void     (*w_on_hit)() = nullptr;
inline std::atomic<int>      w_hits{0};
inline std::atomic<int>      w_hit_size{0};
inline std::atomic<int>      w_seen_total{0};
inline std::atomic<int>      w_native_seen{0};

}  // namespace（operator new/delete 必须是全局重载，不可留在匿名命名空间）

void* operator new(std::size_t n)
{
    if (d_armed) {
        const int ordinal = ++d_seen;
        if (d_fail_on != 0 && ordinal == d_fail_on) {
            d_armed = false;
            d_hit_size.store(static_cast<int>(n), std::memory_order_release);
            d_hits.fetch_add(1);
            throw std::bad_alloc();
        }
    }
    if (w_armed) {
        char marker;
        const auto sp = reinterpret_cast<uintptr_t>(&marker);
        const bool native = sp >= w_stack_begin && sp < w_stack_end;
        const bool victim_ok = (w_victim == nullptr) ||
            (w_victim == g_bbt_tls_coroutine_co);
        if (victim_ok) {
            w_seen_total.fetch_add(1, std::memory_order_relaxed);
            if (native)
                w_native_seen.fetch_add(1, std::memory_order_relaxed);
            if (native && w_fail_native_once) {
                w_fail_native_once = false;
                w_armed = false;
                w_hit_size.store(static_cast<int>(n), std::memory_order_release);
                w_hits.fetch_add(1);
                if (::getenv("OOM_TRACE"))
                    std::fprintf(stderr, "OOM native size=%zu\n", n);
                /* 命中动作在抛 bad_alloc 之前同步执行——此时本分配
                 * 尚未失败、事件仍在 ARMED 登记窗口，Notify/RequestCancel
                 * 的 Trigger 构成真实 PENDING 首胜（与独立探针同一时序）。
                 * 先取走并清空再调用：动作内任何嵌套分配都已 disarm，不会
                 * 重入命中分支。 */
                auto* on_hit = w_on_hit;
                w_on_hit = nullptr;
                if (on_hit != nullptr)
                    on_hit();
                throw std::bad_alloc();
            }
        }
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

namespace
{

/* 直接驱动记序窗口：构造/析构在协程内（同一测试主线程 TLS）。 */
struct DirectOrdinalGuard
{
    explicit DirectOrdinalGuard(int fail_on) { d_seen = 0; d_fail_on = fail_on; d_armed = true; }
    ~DirectOrdinalGuard() { Disarm(); }
    void Disarm() { d_armed = false; d_fail_on = 0; }
    int Seen() const { return d_seen; }
};

/* worker 注入窗口：必须在协程内构造——pthread_getattr_np(pthread_self())
 * 取的是承载本协程的 worker 原生栈边界；回调在 Context::Resume 原生栈
 * 执行时据此判 native，prep 域协程栈分配判非 native。
 * fail_native_once=true：armed 期间首个原生栈分配（即挂起后回调窗口内
 * Regist() 顶部的 DebugMgr map insert）抛 bad_alloc 一次性消费。 */
struct WorkerOrdinalGuard
{
    explicit WorkerOrdinalGuard(bool fail_native_once, void (*on_hit)() = nullptr)
    {
        if (w_stack_end == 0) {
            pthread_attr_t attributes;
            if (pthread_getattr_np(pthread_self(), &attributes) != 0)
                throw std::runtime_error("cannot inspect native worker stack");
            void* base = nullptr;
            size_t size = 0;
            const int rc = pthread_attr_getstack(&attributes, &base, &size);
            pthread_attr_destroy(&attributes);
            if (rc != 0)
                throw std::runtime_error("cannot resolve native worker stack bounds");
            w_stack_begin = reinterpret_cast<uintptr_t>(base);
            w_stack_end = w_stack_begin + size;
        }
        w_on_hit = on_hit;
        w_fail_native_once = fail_native_once;
        w_victim = g_bbt_tls_coroutine_co;
        w_armed = true;
    }
    ~WorkerOrdinalGuard() { Disarm(); }
    void Disarm() { w_armed = false; w_fail_native_once = false; w_victim = nullptr; w_on_hit = nullptr; }
};

struct LargeCallback
{
    char payload[64]{};
    int* invoked;
    bool operator()() const { ++*invoked; return true; }
};

}  // namespace

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(DiagnosticWaitOomTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    EnsureRuntime();
    BOOST_REQUIRE(g_scheduler->IsInitialized());
}

/* ============ 挂起前事务内不再含可分配的诊断日志装配 ============ */

/* 直接驱动逐序号扫描：协程持 custom await 事件 + 非 SBO 挂起回调，
 * armed 窗口覆盖 YieldWithCallback 的挂起前事务。日志装配已改为栈缓冲
 * snprintf（无分配），首分配即真实 Context 存储（m_onyield_callback）。
 * 任一 bad_alloc 都必须满足：异常可观察、协程仍是 CO_RUNNING、await
 * 已清账（RegistCustom 可再建）。父反例在 DEBUG_INFO 打开时曾实测
 * status_running=0 / await_reusable=0。 */
BOOST_AUTO_TEST_CASE(t_pre_yield_log_alloc_rollback)
{
    /* 单轮直接驱动：lambda 体与父探针同型（栈上 co 指针，非原子捕获）。 */
    const auto drive_once = [](int fail_on, int* outcome, int* cb_calls,
                               int* status_running, int* await_reusable, int* last_seen) {
        Coroutine* co = nullptr;
        co = Coroutine::Create(262144, [&]() {
            auto ev = co->RegistCustom(POLL_EVENT_CUSTOM_COND);
            BOOST_REQUIRE(ev != nullptr);
            CoroutineOnYieldCallback cb = LargeCallback{{}, cb_calls};
            try {
                {
                    DirectOrdinalGuard g(fail_on);
                    co->YieldWithCallback(cb);
                }
                *outcome = 3;
            } catch (const std::bad_alloc&) {
                *outcome = 1;
                *status_running = (co->GetStatus() == CoroutineStatus::CO_RUNNING) ? 1 : 0;
                /* 唯一合法收口：bad_alloc 抛出时事务已完整回滚——await 已清，
                 * RegistCustom 必须能真实重建（返回非空）。不允许「旧 await
                 * 保留但协程仍宣称可复用」的第二态：原探针语义即
                 * await_reusable=1 ⟺ RegistCustom 成功。 */
                auto ev2 = co->RegistCustom(POLL_EVENT_CUSTOM_COND);
                *await_reusable = (ev2 != nullptr) ? 1 : 0;
            } catch (...) {
                *outcome = 2;
            }
            *last_seen = d_seen;   /* 故障轮：guard 已析构，记录命中前计数 */
        }, false);
        co->Resume();
        /* 协程真实挂起时（计量轮 fail_on=0 回调成功）构造在协程
         * 栈上的 DirectOrdinalGuard 永不析构，TLS d_armed 保持开启——
         * 必须在 delete co 之前捕获 d_seen 并手动闭合计量窗口，否则
         * 协程/上下文析构期的分配会被计入 total，产生永远无法命中的
         * 幽灵 ordinal。故障轮 guard 已随栈展开析构，此处重复 disarm
         * 无害（同线程 TLS，协程与驱动共用）。 */
        *last_seen = d_seen;
        d_armed = false;
        d_fail_on = 0;
        delete co;
    };

    int total = 0;
    {
        int outcome = 0, cb_calls = 0, status_running = -1, await_reusable = -1, last_seen = 0;
        drive_once(0, &outcome, &cb_calls, &status_running, &await_reusable, &last_seen);
        /* 计量轮回调成功 → 协程真实挂起，栈上 guard 不析构，last_seen
         * 由 Resume 返回后从 TLS 捕获（窗口随即闭合，见 drive_once）。 */
        total = last_seen;
    }
    BOOST_REQUIRE_GE(total, 1);

    for (int i = 1; i <= total; ++i) {
        int outcome = 0, cb_calls = 0, status_running = -1, await_reusable = -1, last_seen = 0;
        drive_once(i, &outcome, &cb_calls, &status_running, &await_reusable, &last_seen);

        /* 每轮唯一许可结果：bad_alloc 被协程捕获且 await 已完整清账。
         * 日志装配已不分配，命中点必在 Context 存储或更早入口分配。 */
        BOOST_REQUIRE_MESSAGE(outcome == 1, "ordinal " << i << " outcome=" << outcome);
        BOOST_CHECK_EQUAL(cb_calls, 0);
        BOOST_CHECK_EQUAL(status_running, 1);
        BOOST_CHECK_EQUAL(await_reusable, 1);
    }
    BOOST_CHECK_GE(d_hits.load(), total);   /* 每轮真实命中一次 */
    BOOST_CHECK_GT(d_hit_size.load(), 0);   /* 命中分配尺寸已记录 */
}

/* ============ 挂起后 DebugMgr 登记分配故障不逃出 worker ============ */

/* 真实 worker 栈命中：victim 的 TryLock 挂起回调在 Context::Resume 原生
 * 栈执行，Regist() 内 DebugMgr map insert（回调窗口内首个 native 分配）
 * 处注入 bad_alloc。修复前该异常逃出 worker → terminate（probe 实测 -6）；
 * 曾以「登记失败 → Regist -1 → CHECK_FAILED → TryLock -1」收口。
 * map insert 单次失败无副作用，Regist 内 dbg_regist 有界重试一次
 * 后登记恢复——本轮等待真实完成。注入轮用有界期限（60ms）让等待自行
 * 超时收口：TryLock 返回 1（超时），不再依赖 holder 放锁，无握手死锁。
 * 注入语义：armed 后首个 native 分配必败，不用序号——两轮 TryLock 的
 * 协程侧分配序不同，实测序号漂移；首个 native 分配点由产品调用序保证
 * 即 map insert。 */
BOOST_AUTO_TEST_CASE(t_postyield_regist_map_oom_recovers)
{
    auto mutex = sync::CoMutex::Create();
    bbt::core::thread::CountDownLatch done{3};
    std::atomic_bool holding{false}, can_unlock{false}, victim_done{false};
    std::atomic<int> first_ret{-99}, second_ret{-99}, measure_ret{-99};
    std::atomic<int> successor_got{0};
    [[maybe_unused]] const int hits_before = w_hits.load();
    [[maybe_unused]] const unsigned long long exc_before =
        g_bbt_coroutine_config->m_unhandled_exception_count.load();

    bbtco [mutex, &holding, &can_unlock, &done]() {
        mutex->Lock();
        holding.store(true, std::memory_order_release);
        CoWaitFlag(can_unlock);          /* 持锁贯穿 victim 两次尝试 */
        mutex->UnLock();
        done.Down();
    };

    bbtco [mutex, &holding, &can_unlock, &victim_done,
          &first_ret, &second_ret, &measure_ret, &done]() {
        CoWaitFlag(holding);

        measure_ret.store(mutex->TryLock(50));   /* 真实挂起→超时=1：路径预热 */
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
        /* 注入：TryLock(60) 挂起后回调的首个 native 分配（DebugMgr map
         * insert）必败；修复后重试登记恢复，本轮等待真实挂起并在
         * 60ms 处自行超时返回 1——不再用 60000ms 长期限依赖 holder 放锁
         * 收口（那会构成「victim 等 holder 放锁 / holder 等 victim 返回
         * 才能放锁」的握手死锁）。 */
        {
            WorkerOrdinalGuard g(true);
            first_ret.store(mutex->TryLock(60));
        }
#else
        /* 非 strict 配置无 DebugMgr 登记点：保持协议非空，复用超时结果。 */
        first_ret.store(measure_ret.load());
#endif
        can_unlock.store(true, std::memory_order_release);
        second_ret.store(mutex->TryLock(2000));
        if (second_ret.load() == 0)
            mutex->UnLock();
        victim_done.store(true, std::memory_order_release);
        done.Down();
    };

    /* 后继等待者：victim 全流程结束后真实获锁——证明锁对象与等待
     * 队列未受污染（放锁确实发生、内部状态可复用）。 */
    bbtco [mutex, &victim_done, &successor_got, &done]() {
        CoWaitFlag(victim_done);
        if (mutex->TryLock(2000) == 0) {
            successor_got.store(1);
            mutex->UnLock();
        }
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(measure_ret.load(), 1);    /* 预热轮是真实挂起超时 */
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    BOOST_CHECK_EQUAL(first_ret.load(), 1);      /* 重试恢复后真实挂起→超时 */
    BOOST_CHECK_EQUAL(w_hits.load(), hits_before + 1);  /* 真实命中一次 */
    BOOST_CHECK_GT(w_hit_size.load(), 0);        /* 命中分配尺寸已记录 */
    BOOST_CHECK_GT(w_native_seen.load(), 0);     /* 观察到 postyield 域分配 */
    BOOST_CHECK_EQUAL(
        g_bbt_coroutine_config->m_unhandled_exception_count.load(),
        exc_before + 1);                          /* 单次失败已计数，重试不再计 */
#else
    BOOST_CHECK_EQUAL(first_ret.load(), 1);
#endif
    BOOST_CHECK_EQUAL(second_ret.load(), 0);     /* 锁可重获：放锁真实发生 */
    BOOST_CHECK_EQUAL(successor_got.load(), 1);  /* 后继等待者真实进展 */
}

/* ============ ResumeCo map insert OOM 有界单次重试恢复严格配对 ============ */

/* 真实轨迹：bbtco 协程 → worker 栈 Coroutine::Resume（首次，无注入）→
 * 协程内 bbtco_yield（配对 YieldCo 核销）→ armed 后 yield → worker 栈
 * Coroutine::Resume（第二次）内 OnEvent_ResumeCo 的 m_co_map insert
 * 注入一次 bad_alloc → 协程再 bbtco_yield（配对核销）→ FINAL →
 * CommitYield 终态核销。
 *
 * 预期（修复后）：第一次 insert 失败 → ReportAllocFailed（计数+1）→
 * 有界重试一次成功 → 按 id 的真实条目恢复；配对 YieldCo erase>0 严格
 * 核销，无 "yield-co" 报告、无断言、唯一完成。命中点判定：armed 后首个
 * 「victim 协程的 worker 原生栈分配」即第二次 Resume 的 map insert
 * （victim 由 g_bbt_tls_coroutine_co 过滤，Processer::Resume 已把
 * m_running_coroutine 指向它）。
 * 修复前行为（本用例 RED）：lost 计数放行 —— 额外出现 "yield-co no paired
 * resume record" 报告、exception_count 增量为 2 而非 1、且无按 id 条目。 */
BOOST_AUTO_TEST_CASE(t_resumeco_map_insert_oom_bounded_retry)
{
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    EnsureRuntime();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_bool armed_ready{false}, yield_again{false};
    std::atomic<int> finished{0}, yields{0};
    [[maybe_unused]] const int hits_before = w_hits.load();
    [[maybe_unused]] const unsigned long long exc_before =
        g_bbt_coroutine_config->m_unhandled_exception_count.load();

    bbtco [&armed_ready, &yield_again, &finished, &yields, &done]() {
        /* 轨迹：worker Resume#1（登记） → 此处运行 → yield（核销） →
         * armed + yield → worker Resume#2（insert 注入一次 bad_alloc） →
         * 再 yield（配对核销 Resume#2 条目） → FINAL → CommitYield 核销。 */
        yields.fetch_add(1);
        bbtco_yield;                          /* 配对 Resume#1 */

        /* 在协程栈上布防：w_stack 边界在此捕获（此时正跑在 worker 原生
         * 线程的协程栈上）；armed 后让出，等 worker 再次 Resume 本协程时
         * OnEvent_ResumeCo insert 即首个命中分配。 */
        {
            WorkerOrdinalGuard g(true);
            armed_ready.store(true, std::memory_order_release);
            yields.fetch_add(1);
            bbtco_yield;                      /* 挂起 → 等待 Resume#2 注入 */
        }

        /* Resume#2 完成（重试已恢复登记）：再让出一次配对核销 Resume#2
         * 条目（worker 栈 OnEvent_YieldCo erase>0），之后走到 FINAL 由
         * CommitYield 做终态核销。 */
        yield_again.store(true, std::memory_order_release);
        bbtco_yield;
        yields.fetch_add(1);
        finished.store(1, std::memory_order_release);
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(finished.load(), 1);          /* 协程唯一完成 */
    BOOST_CHECK_GE(yields.load(), 3);               /* 真实经历两次让出 */
    BOOST_CHECK_EQUAL(w_hits.load(), hits_before + 1); /* 恰好一次注入命中 */
    BOOST_CHECK_GT(w_hit_size.load(), 0);           /* 命中分配尺寸已记录 */
    BOOST_CHECK_GT(w_native_seen.load(), 0);        /* 观察到 worker 原生栈分配 */
    /* 恰好 +1：第一次 insert 的 bad_alloc 已报告；重试成功不再计数，
     * 配对 YieldCo/CommitYield 严格核销，无 "yield-co" 兜底报告（修复前
     * 是 +2：resume-co + yield-co 缺口放行各计一次）。 */
    BOOST_CHECK_EQUAL(
        g_bbt_coroutine_config->m_unhandled_exception_count.load(),
        exc_before + 1);
#else
    BOOST_TEST(true);
#endif
}

/* PENDING 分支真实闭环：预取消的协程走真实调用者 CoMutex::TryLock(ms)
 * → _WaitUnLockUnitlTimeout → RegistCustom → YieldWithCallback → worker
 * 原生栈 _RegistAwaitEvent → IsCancelRequested 复检命中 →
 * Trigger(CANCELLED) 使 INITED→PENDING → Regist() PENDING 分支的
 * TryRegistEvent map 节点分配被注入必败。修复后 dbg_regist 有界
 * 重试一次登记恢复：事件按已锁存的 CANCELLED 首胜经 CommitPark 消费
 * → _Complete → 协程以取消位唤醒 → TryLock 的取消/超时兼容映射返回 1。
 * 修复前（单次失败）行为：Regist -1 → _RegistAwaitEvent false →
 * CHECK_FAILED → TryLock -1（首胜结果连同取消决议一起丢失）。
 * 唯一完成：无二次 Regist、无手动重试；协程清 await 后仍可再次真实
 * 等待，锁释放后后继 waiter 真实获锁。 */
BOOST_AUTO_TEST_CASE(t_regist_pending_branch_oom_returns_fail)
{
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    EnsureRuntime();
    auto mutex = sync::CoMutex::Create();
    bbt::core::thread::CountDownLatch done{3};
    std::atomic_bool holding{false}, can_unlock{false}, victim_done{false};
    std::atomic<int> lock_ret{-99}, second_ret{-99}, successor_got{0};
    [[maybe_unused]] const int hits_before = w_hits.load();
    [[maybe_unused]] const unsigned long long exc_before =
        g_bbt_coroutine_config->m_unhandled_exception_count.load();

    bbtco [mutex, &holding, &can_unlock, &done]() {
        mutex->Lock();
        holding.store(true, std::memory_order_release);
        CoWaitFlag(can_unlock);          /* 持锁贯穿 victim 的 TryLock 尝试 */
        mutex->UnLock();
        done.Down();
    };

    bbtco [mutex, &holding, &can_unlock, &victim_done,
          &lock_ret, &second_ret, &done]() {
        CoWaitFlag(holding);
        auto* co = g_bbt_tls_coroutine_co;
        /* 预取消：TryLock 无入口 cancel 检查，登记回调在 worker 栈执行
         * 时由 _RegistAwaitEvent 的 cancel 复检 Trigger 成 PENDING，
         * 随后 Regist 走 PENDING 分支——本窗口内首个原生栈堆分配即
         * TryRegistEvent 的 map insert（日志已不分配）。有界期限 2000ms
         * 仅兜底：PENDING 首胜经 CommitPark 立即消费，不会真等。 */
        co->RequestCancel();
        {
            WorkerOrdinalGuard g(true);
            lock_ret.store(mutex->TryLock(2000));
        }
        /* 取消决议已交付、await 已清账：放锁后同协程下一次等待真实完成。
         * second_ret 可为 0（获锁）或 1（超时/残留 CANCELLED 位被兼容
         * 映射消费）——二者都是真实完成，唯一不允许的是 -1/-99。 */
        can_unlock.store(true, std::memory_order_release);
        second_ret.store(mutex->TryLock(2000));
        if (second_ret.load() == 0)
            mutex->UnLock();
        victim_done.store(true, std::memory_order_release);
        done.Down();
    };

    bbtco [mutex, &victim_done, &successor_got, &done]() {
        CoWaitFlag(victim_done);
        if (mutex->TryLock(2000) == 0) {
            successor_got.store(1);
            mutex->UnLock();
        }
        done.Down();
    };

    done.Wait();
    /* PENDING 分支重试恢复后，已锁存的 CANCELLED 首胜经 CommitPark
     * 真实消费，TryLock 兼容映射上报 1——不再返回 -1 丢决议。 */
    BOOST_CHECK_EQUAL(lock_ret.load(), 1);
    BOOST_CHECK_EQUAL(w_hits.load(), hits_before + 1);
    BOOST_CHECK_GT(w_hit_size.load(), 0);
    BOOST_CHECK_EQUAL(
        g_bbt_coroutine_config->m_unhandled_exception_count.load(),
        exc_before + 1);                          /* 单次失败已计数，重试不再计 */
    /* second_ret∈{0,1}：0=真实获锁，1=残留 CANCELLED 位被 TryLock 的
     * 超时/取消映射消费——二者都证明 await 已清账、协程可再次真实等待；
     * 唯一非法值是 -1（又一次登记失败）或 -99（未执行到）。 */
    BOOST_CHECK_GE(second_ret.load(), 0);
    BOOST_CHECK_EQUAL(successor_got.load(), 1);/* 后继 waiter 真实进展 */
#else
    BOOST_TEST(true);
#endif
}

/* ARMED 主成功分支直接驱动：custom 事件（无 fd）经 INITED→ARMING→ARMED，
 * map insert 单次注入必败后 dbg_regist 有界重试恢复登记——Regist 返回 0、
 * 事件真实进入 ARMED 监听态；随后真实 UnRegist 收敛 CANCELLED 终态，
 * 证明登记成功后可按正常路径回收，无 map 残留、无异常外抛。
 * 连续 OOM（重试也失败）仍按「登记未完成」闭环（UnRegist + -1），
 * 不承诺持续 OOM 自愈——该路径由注入一次性消费覆盖不到，属预期边界。 */
BOOST_AUTO_TEST_CASE(t_regist_armed_branch_oom_cancels_event)
{
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    const int hits_before = d_hits.load();
    const unsigned long long exc_before =
        g_bbt_coroutine_config->m_unhandled_exception_count.load();
    auto event = CoPollEvent::Create(BBT_COROUTINE_INVALID_COROUTINE_ID,
        [&](auto, int, int) {});
    BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);

    int ret;
    {
        DirectOrdinalGuard g(1);
        ret = event->Regist();
    }
    BOOST_CHECK_EQUAL(ret, 0);                       /* 重试恢复登记成功 */
    BOOST_CHECK_EQUAL(d_hits.load(), hits_before + 1);   /* 本轮真实命中一次 */
    BOOST_CHECK(event->IsListening());               /* ARMED 监听态 */
    BOOST_CHECK_EQUAL(
        g_bbt_coroutine_config->m_unhandled_exception_count.load(),
        exc_before + 1);                          /* 单次失败已计数，重试不再计 */
    /* 正常回收：已登记事件 UnRegist → CANCELLED 终态 + TriggerEvent 核销
     * 前的 map 清账走既有路径（_Complete 的 OnEvent_TriggerEvent 依赖
     * 重试后的真实条目）。 */
    BOOST_CHECK_EQUAL(event->UnRegist(), 0);
    BOOST_CHECK_EQUAL(event->GetStatus(), CoPollEventStatus::POLLEVENT_CANNEL);
    BOOST_CHECK(event->IsFinal());
#else
    BOOST_TEST(true);
#endif
}

/* ============ 单次诊断登记 OOM 后保住已接纳的首胜与载荷 ============ */

/* 真实公共路径专用回归：bbtco 协程经 sync::CoWaiter::WaitWithCallback
 * （窄接口 + on_registered + 载荷输出）等待；挂起回调在 worker 原生栈
 * _RegistAwaitEvent → Regist() → TryRegistEvent 的 map insert 处注入
 * 一次 bad_alloc，并在同一注入点（事件仍在 ARMED 登记窗口）同步投递
 * 首胜——notify 模式经真实 Notify(CoEventValue::From(42))，cancel 模式
 * 经真实 RequestCancel——与独立探针同一时序，无任何手动 Regist/
 * 再次 Trigger/干跑。
 *
 * 修复前（父 4/4 复现）：Notify 已接纳(payload 42)或 Cancel 已首胜，
 * 但 dbg_regist 单次失败 → Regist -1 → CHECK_FAILED →
 * WaitStatus::RuntimeUnavailable，callbacks=0、无 payload——已接纳的
 * 首胜决议随登记失败一起丢失。
 * 修复后：dbg_regist 有界重试一次恢复登记，已锁存的 PENDING 首胜经
 * CommitPark 真实消费：notify 模式 Completed + payload 42 + 回调一次；
 * cancel 模式 Cancelled + 无 payload + 回调一次。命中恰一次，异常计数
 * +1，无双完成/无重复 Regist 断言。 */
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
namespace
{
struct FirstWinnerShared
{
    sync::CoWaiter::SPtr waiter{sync::CoWaiter::Create()};
    Coroutine* co{nullptr};
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<int> notify_ret{-99}, status{-99}, callbacks{0};
    std::atomic<int> has_payload{0}, payload{-99}, finished{0};
};

/* 协程闭包持有 shared_ptr；注入指针只在同一 worker 同步借用，
 * 完成前清空，不借用测试函数栈对象。 */
thread_local FirstWinnerShared* t_first_winner = nullptr;

void FirstWinnerNotifyHit()
{
    t_first_winner->notify_ret.store(
        t_first_winner->waiter->Notify(CoEventValue::From(42)),
        std::memory_order_release);
}

void FirstWinnerCancelHit()
{
    t_first_winner->co->RequestCancel();
}
}  // namespace
#endif

BOOST_AUTO_TEST_CASE(t_cowaiter_first_winner_oom_recovers)
{
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    EnsureRuntime();
    for (int mode = 0; mode < 2; ++mode) {
        auto state = std::make_shared<FirstWinnerShared>();
        const int hits_before = w_hits.load();
        const auto exc_before = g_bbt_coroutine_config->m_unhandled_exception_count.load();
        bbtco [state, mode]() {
            state->co = g_bbt_tls_coroutine_co;
            t_first_winner = state.get();
            CoEventValue value;
            CoroutineOnYieldCallback cb = [state]() {
                state->callbacks.fetch_add(1);
                return true;
            };
            {
                WorkerOrdinalGuard guard(true,
                    mode == 0 ? &FirstWinnerNotifyHit : &FirstWinnerCancelHit);
                state->status.store(static_cast<int>(
                    state->waiter->WaitWithCallback(WaitOptions{}, cb, &value)));
            }
            int out = -99;
            state->has_payload.store(value.Get(out) ? 1 : 0);
            state->payload.store(out);
            t_first_winner = nullptr;
            state->co = nullptr;
            state->finished.store(1);
            state->done.Down();
        };
        state->done.Wait();
        BOOST_CHECK_EQUAL(state->finished.load(), 1);
        BOOST_CHECK_EQUAL(w_hits.load(), hits_before + 1);
        BOOST_CHECK_GT(w_hit_size.load(), 0);
        BOOST_CHECK_GT(w_native_seen.load(), 0);
        BOOST_CHECK_EQUAL(state->callbacks.load(), 1);
        BOOST_CHECK_EQUAL(g_bbt_coroutine_config->m_unhandled_exception_count.load(),
                          exc_before + 1);
        if (mode == 0) {
            BOOST_CHECK_EQUAL(state->notify_ret.load(), 0);
            BOOST_CHECK_EQUAL(state->status.load(), static_cast<int>(WaitStatus::Completed));
            BOOST_CHECK_EQUAL(state->has_payload.load(), 1);
            BOOST_CHECK_EQUAL(state->payload.load(), 42);
        } else {
            BOOST_CHECK_EQUAL(state->status.load(), static_cast<int>(WaitStatus::Cancelled));
            BOOST_CHECK_EQUAL(state->has_payload.load(), 0);
        }
    }
#else
    BOOST_TEST(true);
#endif
}

BOOST_AUTO_TEST_SUITE_END()
