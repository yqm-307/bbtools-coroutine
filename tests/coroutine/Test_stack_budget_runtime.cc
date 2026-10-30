/**
 * @file Test_stack_budget_runtime.cc
 * @brief 可选栈采样诊断的运行时回归（BBT_COROUTINE_STACK_DIAGNOSTICS
 * 构建才注册，见 tests/CMakeLists.txt 末尾诊断块）。
 *
 * 断言口径固定为 PARTIAL 观测：
 *  - 深路径 observed > 浅路径（同栈尺寸、同编译单元插桩）；
 *  - 样本随 Context 持有，协程返回后仍可读；复用栈不继承历史；
 *  - 跨线程 Resume / 多线程并发 / 真实 Scheduler 多协程下归属隔离无竞态；
 *  - 未插桩 TU 与 mid-function alloca 只断言 PARTIAL，不声称观测到真实峰值。
 *
 * 并发纪律：每个协程的 sink 是它自己栈帧上的 volatile char（生命周期覆盖
 * 全部 Burn 调用），不跨协程共享，不用 atomic——避免在观测路径上引入
 * 与真实负载无关的帧差异和额外插桩调用。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>

#include <bbt/core/thread/lock/CountDownLatch.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Context.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/debug/StackBudgetDiagnostics.hpp>

/* 进程寿命模型：runtime 只初始化一次。 */
namespace
{
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, [](){
        bbt::coroutine::detail::Scheduler::GetInstance()->Start();
    });
}
}

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

namespace
{

constexpr size_t  kStackSize    = 64 * 1024;
constexpr int     kShallowDepth = 1;
constexpr int     kDeepDepth    = 24;

/* 插桩路径上的确定性栈消耗：每层一个固定 volatile 帧，递归深度即栈深。
 * noinline 保证每层都是真实帧，不会被常量传播折叠。sink 由调用方提供，
 * 落在该协程自己的栈帧上。 */
__attribute__((noinline))
void Burn(volatile char* sink, int depth)
{
    volatile char frame[384];
    for (unsigned i = 0; i < sizeof(frame); i += 64)
        frame[i] = static_cast<char>(i + depth);
    *sink = static_cast<char>(*sink + frame[0]);

    if (depth > 1)
        Burn(sink, depth - 1);
}

/* mid-function alloca 之后没有进一步的插桩调用：入口样本（alloca 前的 SP）
 * 观测不到这块延伸，只能证明 PARTIAL 覆盖，不能当真实峰值。 */
__attribute__((noinline))
void AllocaThenNoInstrumentedCall(volatile char* sink)
{
    volatile char* p = static_cast<volatile char*>(__builtin_alloca(8 * 1024));
    for (int i = 0; i < 8 * 1024; i += 64)
        p[i] = static_cast<char>(i);
    *sink = static_cast<char>(*sink + p[0]);
}

} // namespace

namespace bbt_stack_budget_test
{
void UninstrumentedBurn(volatile char* sink, int depth);
}

BOOST_AUTO_TEST_SUITE(stack_budget_runtime)

/* 1) 不 yield 的深调用：返回后样本保留，深路径 observed 大于浅路径。 */
BOOST_AUTO_TEST_CASE(t_deep_observed_gt_shallow_and_retained)
{
    Context deep_ctx(kStackSize, []() {
        volatile char sink{0};
        Burn(&sink, kDeepDepth);
    }, true);
    deep_ctx.Resume();
    const auto deep = deep_ctx.GetStackBudgetSample();

    Context shallow_ctx(kStackSize, []() {
        volatile char sink{0};
        Burn(&sink, kShallowDepth);
    }, true);
    shallow_ctx.Resume();
    const auto shallow = shallow_ctx.GetStackBudgetSample();

    BOOST_TEST(static_cast<int>(deep.coverage) == static_cast<int>(StackBudgetCoverage::PARTIAL));
    BOOST_TEST(static_cast<int>(shallow.coverage) == static_cast<int>(StackBudgetCoverage::PARTIAL));
    BOOST_TEST(deep.sample_count > 0);
    BOOST_TEST(shallow.sample_count > 0);
    BOOST_TEST(deep.observed_sample_bytes > shallow.observed_sample_bytes);

    /* 协程结束后安全点快照仍可读且不回落 */
    BOOST_TEST(deep_ctx.GetStackBudgetSample().observed_sample_bytes ==
               deep.observed_sample_bytes);
    BOOST_TEST(deep_ctx.GetStackBudgetSample().sample_count ==
               deep.sample_count);
}

/* 2) 手工 Context 跨线程 Resume：TLS 归属随 owner 迁移，两线程均无残留。 */
BOOST_AUTO_TEST_CASE(t_tls_ownership_migrates_across_threads)
{
    std::unique_ptr<Context> ctx;
    std::atomic_bool a_clean{false};
    std::atomic_bool b_clean{false};

    ctx.reset(new Context(kStackSize, [&]() {
        volatile char sink{0};
        Burn(&sink, 8);
        ctx->Yield();
        Burn(&sink, kDeepDepth);
    }, true));

    std::thread ta([&]() {
        ctx->Resume();
        a_clean = (GetActiveStackBudgetState() == nullptr);
    });
    ta.join();

    const auto mid = ctx->GetStackBudgetSample();
    BOOST_TEST(mid.sample_count > 0);

    std::thread tb([&]() {
        ctx->Resume();
        b_clean = (GetActiveStackBudgetState() == nullptr);
    });
    tb.join();

    const auto end = ctx->GetStackBudgetSample();
    BOOST_TEST(static_cast<int>(end.coverage) == static_cast<int>(StackBudgetCoverage::PARTIAL));
    /* 样本单调：迁移后不回退，且第二段更深的 Burn 推进水位 */
    BOOST_TEST(end.sample_count >= mid.sample_count);
    BOOST_TEST(end.observed_sample_bytes >= mid.observed_sample_bytes);
    BOOST_TEST(a_clean.load());
    BOOST_TEST(b_clean.load());
    BOOST_TEST(GetActiveStackBudgetState() == nullptr);
}

/* 3) YieldWithCallback 失败重入：让出期间 worker 侧无归属，重入后本协程
 * 状态恢复、回调本身不产生本协程样本。 */
BOOST_AUTO_TEST_CASE(t_yieldwithcallback_failed_reentry_keeps_state)
{
    std::unique_ptr<Context> ctx;
    StackBudgetState* self_state{nullptr};
    StackBudgetState* seen_in_callback{reinterpret_cast<StackBudgetState*>(~uintptr_t{0})};
    int               yield_cb_ret{0};
    bool              active_restored{false};

    ctx.reset(new Context(kStackSize, [&]() {
        volatile char sink{0};
        Burn(&sink, 8);
        self_state = GetActiveStackBudgetState();
        /* callback 在 worker 栈上执行：此刻协程已让出，TLS 归属必须为 nullptr */
        yield_cb_ret = ctx->YieldWithCallback([&]() -> bool {
            seen_in_callback = GetActiveStackBudgetState();
            return false;   /* check 失败 → Resume 侧重入本协程 */
        });
        /* 重入后归属应重新指向本协程同一状态（不是别的对象、不是空） */
        active_restored = (GetActiveStackBudgetState() == self_state);
        Burn(&sink, 8);
    }, true));

    ctx->Resume();

    BOOST_TEST(self_state != nullptr);
    BOOST_TEST(seen_in_callback == nullptr);
    BOOST_TEST(yield_cb_ret == -1);
    BOOST_TEST(active_restored);

    const auto s = ctx->GetStackBudgetSample();
    BOOST_TEST(static_cast<int>(s.coverage) == static_cast<int>(StackBudgetCoverage::PARTIAL));
    BOOST_TEST(s.sample_count > 0);
    BOOST_TEST(GetActiveStackBudgetState() == nullptr);
}

/* 4) 异常展开/RAII 与终态后的安全点快照：仍 PARTIAL 且有样本。 */
BOOST_AUTO_TEST_CASE(t_exception_unwind_snapshot_partial_with_samples)
{
    bool dtor_ran = false;
    struct Guard {
        bool& flag;
        ~Guard() { flag = true; }
    };

    Context ctx(kStackSize, [&]() {
        volatile char sink{0};
        Burn(&sink, 8);
        Guard g{dtor_ran};
        Burn(&sink, 4);
        throw std::runtime_error("stack_budget_exception_probe");
    }, true);

    /* 异常在 _CoroutineMain 边界被捕获（detached 路径：日志+计数），
     * 终态 Yield 后 Resume 正常返回。 */
    ctx.Resume();

    BOOST_TEST(dtor_ran);
    const auto s = ctx.GetStackBudgetSample();
    BOOST_TEST(static_cast<int>(s.coverage) == static_cast<int>(StackBudgetCoverage::PARTIAL));
    BOOST_TEST(s.sample_count > 0);
    BOOST_TEST(s.observed_sample_bytes > 0);
}

/* 5) 栈池同尺寸顺序借还 + 确定性物理复用：专用桶键 kStackSize+4096（此前
 * 任何用例未向该键归还栈），本用例在 EnsureRuntime 之前运行、无 scheduler
 * 维护线程——c1 是该桶第一次申请，析构 FIFO 归还后桶内唯一块即它，c2 再借
 * 必然复用同一物理栈。两边捕获活动 state 的 top 相等证明复用已发生，
 * fresh 状态（sample_count/observed==0）由此对「不继承历史」有判别力。 */
BOOST_AUTO_TEST_CASE(t_pool_reuse_fresh_context_no_history)
{
    constexpr size_t kDedicatedStack = kStackSize + 4096;
    uintptr_t top1{0};
    uintptr_t top2{0};

    {
        Context c1(kDedicatedStack, [&]() {
            volatile char sink{0};
            top1 = GetActiveStackBudgetState()->top;
            Burn(&sink, 16);
        }, true);
        c1.Resume();
        BOOST_TEST(c1.GetStackBudgetSample().sample_count > 0);
        BOOST_TEST(top1 != 0);
    }
    /* c1 析构归还栈后再借同尺寸同保护：同键桶里唯一空闲块就是它 */
    Context c2(kDedicatedStack, [&]() {
        volatile char sink{0};
        top2 = GetActiveStackBudgetState()->top;
        Burn(&sink, 4);
    }, true);
    {
        const auto fresh = c2.GetStackBudgetSample();
        BOOST_TEST(fresh.sample_count == 0);
        BOOST_TEST(fresh.observed_sample_bytes == 0);
        BOOST_TEST(static_cast<int>(fresh.coverage) == static_cast<int>(StackBudgetCoverage::PARTIAL));
    }
    c2.Resume();
    BOOST_TEST(top2 == top1);   /* 确定性物理复用：同一块栈，样本不继承 */
    BOOST_TEST(c2.GetStackBudgetSample().sample_count > 0);
}

/* 6) 多 native 线程各自独立 Context 交错 yield：归属完整隔离无竞态。 */
BOOST_AUTO_TEST_CASE(t_multi_thread_independent_contexts_isolated)
{
    constexpr int kThreads = 4;
    constexpr int kRounds  = 6;

    struct Worker {
        std::unique_ptr<Context> ctx;
        std::atomic_bool         done{false};
        std::atomic_bool         tls_clean{false};
        StackBudgetSample        sample{};
        int                      depth{0};
    };

    std::array<Worker, kThreads> workers;
    for (int i = 0; i < kThreads; ++i)
    {
        workers[i].depth = (i % 2 == 0) ? kDeepDepth : kShallowDepth + 3;
        auto* w = &workers[i];
        w->ctx.reset(new Context(kStackSize, [w]() {
            volatile char sink{0};
            for (int r = 0; r < kRounds; ++r)
            {
                Burn(&sink, w->depth);
                w->ctx->Yield();
            }
            w->done = true;
        }, true));
    }

    std::array<std::thread, kThreads> threads;
    for (int i = 0; i < kThreads; ++i)
    {
        auto* w = &workers[i];
        threads[i] = std::thread([w]() {
            while (!w->done.load())
                w->ctx->Resume();
            w->sample = w->ctx->GetStackBudgetSample();
            w->tls_clean = (GetActiveStackBudgetState() == nullptr);
        });
    }
    for (auto& t : threads)
        t.join();

    for (int i = 0; i < kThreads; ++i)
    {
        BOOST_TEST_INFO("worker " << i);
        BOOST_TEST(static_cast<int>(workers[i].sample.coverage) == static_cast<int>(StackBudgetCoverage::PARTIAL));
        BOOST_TEST(workers[i].sample.sample_count > 0);
        BOOST_TEST(workers[i].sample.observed_sample_bytes > 0);
        BOOST_TEST(workers[i].tls_clean.load());
    }
    /* 深/浅组各自独立推进：深组水位显著高于浅组（隔离且各自生效） */
    BOOST_TEST(workers[0].sample.observed_sample_bytes >
               workers[1].sample.observed_sample_bytes);
    BOOST_TEST(GetActiveStackBudgetState() == nullptr);
}

/* 7) 未插桩 TU 与 mid-function alloca：只能断言 PARTIAL，不声称观测到
 * 真实峰值。 */
BOOST_AUTO_TEST_CASE(t_partial_coverage_boundaries)
{
    /* 未插桩 helper：其内部帧不打点，observed 只反映插桩调用方的入口 SP */
    Context c1(kStackSize, []() {
        volatile char sink{0};
        bbt_stack_budget_test::UninstrumentedBurn(&sink, kDeepDepth);
    }, true);
    c1.Resume();
    const auto uninst = c1.GetStackBudgetSample();
    BOOST_TEST(static_cast<int>(uninst.coverage) == static_cast<int>(StackBudgetCoverage::PARTIAL));
    BOOST_TEST(uninst.sample_count > 0);
    /* 深度消耗真实发生但未插桩：observed 必须显著小于插桩深路径，
     * 证明它是 PARTIAL 观测而非真实峰值。 */
    Context c2(kStackSize, []() {
        volatile char sink{0};
        Burn(&sink, kDeepDepth);
    }, true);
    c2.Resume();
    BOOST_TEST(uninst.observed_sample_bytes <
               c2.GetStackBudgetSample().observed_sample_bytes);

    /* mid-function alloca：8KiB 延伸发生在入口打点之后、其后无插桩调用 */
    Context c3(kStackSize, []() {
        volatile char sink{0};
        AllocaThenNoInstrumentedCall(&sink);
    }, true);
    c3.Resume();
    const auto alloca_sample = c3.GetStackBudgetSample();
    BOOST_TEST(static_cast<int>(alloca_sample.coverage) == static_cast<int>(StackBudgetCoverage::PARTIAL));
    BOOST_TEST(alloca_sample.sample_count > 0);
}

/* 8) 真实 Scheduler 多协程 yield/resume 隔离：CountDownLatch 协调，
 * 超时立即 _Exit，避免悬垂进程。 */
BOOST_AUTO_TEST_CASE(t_scheduler_multi_coroutine_isolation)
{
    EnsureRuntime();

    constexpr int kCo = 4;
    bbt::core::thread::CountDownLatch latch(kCo);
    std::array<StackBudgetSample, kCo> samples{};
    std::array<std::atomic_bool, kCo> captured{};

    for (int i = 0; i < kCo; ++i)
    {
        const int deep   = (i % 2 == 0) ? kDeepDepth : kShallowDepth + 2;
        const int deep2  = (i % 2 == 0) ? 8 : kDeepDepth;
        g_scheduler->RegistCoroutineTask(kStackSize, [&, i, deep, deep2]() {
            volatile char sink{0};
            auto* self = g_bbt_tls_coroutine_co;
            Burn(&sink, deep);
            bbtco_yield;
            Burn(&sink, deep2);
            /* owner 协程在自身运行安全点读（同线程同步写，无并发）；
             * 快照拷贝到测试侧，由 latch 建立 happens-before */
            if (self != nullptr) {
                samples[i] = self->GetStackBudgetSample();
                captured[i] = true;
            }
            latch.Down();
        }, "stack_budget_isolation");
    }

    if (latch.WaitTimeout(60000) != 0)
        _Exit(2);

    for (int i = 0; i < kCo; ++i)
    {
        BOOST_TEST_INFO("coroutine " << i);
        BOOST_TEST(captured[i].load());
        BOOST_TEST(static_cast<int>(samples[i].coverage) == static_cast<int>(StackBudgetCoverage::PARTIAL));
        BOOST_TEST(samples[i].sample_count > 0);
        BOOST_TEST(samples[i].observed_sample_bytes > 0);
    }
    /* 深浅两类协程各自推进自己的最深水位 */
    BOOST_TEST(samples[0].observed_sample_bytes > 0);
    BOOST_TEST(samples[1].observed_sample_bytes > 0);
}

BOOST_AUTO_TEST_SUITE_END()
