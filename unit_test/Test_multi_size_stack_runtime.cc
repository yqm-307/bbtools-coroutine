/**
 * 多尺寸固定栈 runtime 验收：薄 main，不引入测试框架。
 *
 * 只有三种模式，各自作为独立 CTest 进程运行（单进程只 Start 一次；运行时是
 * 进程寿命对象，没有 Stop/restart，同进程内不能切换保护模式或驱动模式）：
 *   run-protected       启动前设非默认保护+尺寸，同一 runtime 交错默认/显式/
 *                       带描述/成功输出任务，真实有界挂起唤醒
 *   run-unprotected     同上，保护关闭；确认库在两种保护模式下都工作
 *   reject-before-start 不启动 runtime：非法值在分配前被拒绝（异常类别）、无 worker
 *                       的注册拒绝不留对象、回调不执行、计数回零
 *
 * 断言全在控制线程（主线程）做，协程内只记录观测值并 Down 计数；不在保护/小值
 * 场景执行测试框架本身（极小栈只做内存布局验证，见 Test_coroutine_stack）。
 */
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>

namespace {

/* 只链接进本测试可执行文件，不给库增加 seam。只在未 Start 的主线程按精确大小
 * 一次性拒绝 scalar new；其余分配照常。跳过次数用于先建一个真实栈再模拟 OOM。
 * 描述分配失败时采样已借栈计数，区分确实到达赋值与更早的构造失败。 */
thread_local size_t g_fail_new_size = 0;
thread_local size_t g_skip_matching_new = 0;
thread_local size_t g_borrowed_at_new_failure = 0;
thread_local bbt::coroutine::detail::StackPool* g_observed_pool = nullptr;

} // namespace

void* operator new(std::size_t size)
{
    if (g_fail_new_size != 0 && size == g_fail_new_size)
    {
        if (g_skip_matching_new != 0)
            --g_skip_matching_new;
        else
        {
            g_fail_new_size = 0;
            if (g_observed_pool != nullptr)
                g_borrowed_at_new_failure = g_observed_pool->GetCurCoNum();
            throw std::bad_alloc();
        }
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (ok)
        std::fprintf(stderr, "[multi-size-stack] ok   : %s\n", what);
    else
    {
        std::fprintf(stderr, "[multi-size-stack] FAIL : %s\n", what);
        ++g_failures;
    }
}

size_t AlignUp(size_t request)
{
    const size_t page = static_cast<size_t>(getpagesize());
    return request + ((page - request % page) % page);
}

int RunRuntime(bool stack_protect)
{
    using bbt::core::thread::CountDownLatch;
    using bbt::coroutine::GetLocalCoroutineStackSize;

    auto* cfg = g_bbt_coroutine_config.get();
    const size_t kDefaultStack = 32 * 1024;     // 非出厂默认（12 KiB），用于证明默认值来源
    const size_t kExplicitRequest = 64 * 1024 + 1;
    const size_t kExplicitU = AlignUp(kExplicitRequest);
    const size_t kDescRequest = 16 * 1024;

    cfg->m_cfg_static_thread_num = 1;
    cfg->m_cfg_stack_protect = stack_protect;
    cfg->m_cfg_stack_size = kDefaultStack;

    g_scheduler->Start();
    Check(g_scheduler->IsInitialized(), "runtime initialized");

    std::atomic<size_t> default_seen{0};
    std::atomic<size_t> default_after_resume{0};
    std::atomic<int> default_local_intact{0};
    std::atomic<size_t> default_after_explicit_seen{0};
    std::atomic<size_t> explicit_seen_before_suspend{0};
    std::atomic<size_t> explicit_seen_after_resume{0};
    std::atomic<size_t> desc_seen{0};
    std::atomic<int>    local_intact{0};
    CountDownLatch      done{5};

    /* 默认入口：省略大小必须取自启动前的全局默认，而不是出厂值或被显式调用改写 */
    g_scheduler->RegistCoroutineTask([&]() {
        volatile unsigned char local[1024];
        for (size_t i = 0; i < sizeof(local); ++i)
            local[i] = static_cast<unsigned char>(i * 5 + 11);
        default_seen.store(GetLocalCoroutineStackSize());
        bbt::coroutine::detail::Hook_Sleep(30);
        bool intact = true;
        for (size_t i = 0; i < sizeof(local); ++i)
            if (local[i] != static_cast<unsigned char>(i * 5 + 11))
                intact = false;
        default_after_resume.store(GetLocalCoroutineStackSize());
        default_local_intact.store(intact ? 1 : 0);
        done.Down();
    });

    /* 显式大小 + 描述：真实挂起/唤醒，前后查询值稳定、局部数据未污染 */
    g_scheduler->RegistCoroutineTask(kExplicitRequest, [&]() {
        volatile unsigned char local[4096];
        for (size_t i = 0; i < sizeof(local); ++i)
            local[i] = static_cast<unsigned char>(i * 7 + 3);

        explicit_seen_before_suspend.store(GetLocalCoroutineStackSize());
        bbt::coroutine::detail::Hook_Sleep(30);     // 真实挂起并唤醒

        bool intact = true;
        for (size_t i = 0; i < sizeof(local); ++i)
        {
            if (local[i] != static_cast<unsigned char>(i * 7 + 3))
                intact = false;
        }
        local_intact.store(intact ? 1 : 0);
        explicit_seen_after_resume.store(GetLocalCoroutineStackSize());
        done.Down();
    }, "explicit-64k");

    /* 描述宏：保持原语法，默认大小 */
    bbtco_desc("macro-desc") [&]() {
        desc_seen.store(GetLocalCoroutineStackSize());
        done.Down();
    };

    /* 成功输出重载（显式大小）：不抛并置 true */
    bool succ = false;
    g_scheduler->RegistCoroutineTask(kDescRequest, [&]() { done.Down(); }, succ);
    Check(succ, "explicit RegistCoroutineTask(succ) reports success on live runtime");

    /* 显式调用之后默认入口仍走全局默认 */
    g_scheduler->RegistCoroutineTask([&]() {
        default_after_explicit_seen.store(GetLocalCoroutineStackSize());
        done.Down();
    });

    const bool completed = done.WaitTimeout(15000) == 0;
    Check(completed, "all mixed-size tasks finished within timeout");
    if (!completed)
        std::_Exit(1); // runtime 无 Stop；超时立即结束进程，不能销毁仍被协程引用的局部状态

    Check(default_seen.load() == kDefaultStack,
          "default registration honors configured default U");
    Check(default_after_resume.load() == kDefaultStack,
          "default stack size stable across suspend/resume");
    Check(default_local_intact.load() == 1,
          "default local data intact across suspend/resume");
    Check(default_after_explicit_seen.load() == kDefaultStack,
          "explicit registration does not rewrite the default");
    Check(explicit_seen_before_suspend.load() == kExplicitU,
          "explicit request is honored (aligned U)");
    Check(explicit_seen_after_resume.load() == kExplicitU,
          "explicit stack size stable across suspend/resume");
    Check(local_intact.load() == 1, "local data intact across suspend/resume");
    Check(desc_seen.load() == kDefaultStack, "desc macro keeps default size semantics");
    Check(cfg->m_cfg_stack_size == kDefaultStack && cfg->m_cfg_stack_protect == stack_protect,
          "global config unchanged by explicit registrations");

    std::fprintf(stdout,
        "[multi-size-stack] default=%zu/%zu explicit=%zu/%zu desc=%zu protect=%d\n",
        default_seen.load(), default_after_resume.load(),
        explicit_seen_before_suspend.load(), explicit_seen_after_resume.load(), desc_seen.load(),
        static_cast<int>(stack_protect));
    return 0;
}

void CheckProfileStackEvents(size_t allocations, size_t releases)
{
#ifdef BBT_COROUTINE_PROFILE
    std::string info;
    g_bbt_profiler->ProfileInfo(info);
    char expected[128];
    std::snprintf(expected, sizeof(expected),
                  "StackPool 指标, alloc_count=%zu\trelease_count=%zu\n",
                  allocations, releases);
    Check(info.find(expected) != std::string::npos,
          "PROFILE counts every actual stack allocation and destruction");
    std::fprintf(stderr, "%s", info.c_str());
#else
    (void)allocations;
    (void)releases;
#endif
}

void RunDescriptionAssignFailure()
{
    using bbt::coroutine::detail::Coroutine;
    using bbt::coroutine::detail::CoroutineCallback;
    auto* pool = g_bbt_stackpoll.get();
    const size_t borrowed = pool->GetCurCoNum();
    const size_t allocated = pool->AllocSize(); // 64 KiB 默认保护桶已由无 worker 例预热
    auto capture = std::make_shared<int>(0);
    bool callback_ran = false;
    CoroutineCallback callback = [&, capture]() { callback_ran = true; };
    const auto capture_owners = capture.use_count();
    char desc[8193];
    std::memset(desc, 'd', sizeof(desc) - 1);
    desc[sizeof(desc) - 1] = '\0';

    /* 当前 libstdc++ 的长字符串按 length+1 分配。正对照保持相同 callable/栈/
     * 注入大小，唯独不传 desc：gate 必须未被消费，排除普通构造路径同尺寸分配。 */
    g_skip_matching_new = 0;
    g_fail_new_size = sizeof(desc);
    bool without_desc_succeeded = false;
    try
    {
        std::unique_ptr<Coroutine> control(Coroutine::Create(64 * 1024, callback, true));
        without_desc_succeeded = control->GetStackSize() == 64 * 1024 &&
                                 g_fail_new_size == sizeof(desc);
    }
    catch (const std::bad_alloc&) {}
    g_fail_new_size = 0;
    Check(without_desc_succeeded, "same construction without description does not consume OOM gate");
    Check(pool->GetCurCoNum() == borrowed && pool->AllocSize() == allocated &&
          capture.use_count() == capture_owners && !callback_ran,
          "no-description control returns its stack and callback copies");

    bool rejected = false;
    g_observed_pool = pool;
    g_borrowed_at_new_failure = borrowed;
    g_skip_matching_new = 0;
    g_fail_new_size = sizeof(desc); // 真实 std::string 赋值申请 length+1，不是手工在赋值前抛
    try
    {
        std::unique_ptr<Coroutine> co(Coroutine::Create(64 * 1024, callback, true, desc));
    }
    catch (const std::bad_alloc&) { rejected = true; }
    const bool consumed = g_fail_new_size == 0;
    g_fail_new_size = 0;
    g_observed_pool = nullptr;

    Check(rejected && consumed, "actual description allocation raises bad_alloc");
    Check(g_borrowed_at_new_failure == borrowed + 1,
          "description OOM occurs after the coroutine has borrowed its stack");
    Check(!callback_ran, "description failure does not run or publish the callback");
    Check(capture.use_count() == capture_owners, "description failure destroys callback copies");
    Check(pool->GetCurCoNum() == borrowed && pool->AllocSize() == allocated,
          "description failure returns the borrowed stack without leaking an object");

    std::unique_ptr<Coroutine> co(Coroutine::Create(64 * 1024, callback, true, desc));
    Check(co->GetStackSize() == 64 * 1024, "creation succeeds after one-shot description OOM");
    co->Resume();
    Check(callback_ran, "recovered coroutine executes normally");
    co.reset();
    Check(pool->GetCurCoNum() == borrowed && pool->AllocSize() == allocated &&
          capture.use_count() == capture_owners,
          "recovered coroutine reuses and returns the same sized stack");
}

void RunPreloadFailures()
{
    using bbt::coroutine::detail::Stack;
    using bbt::coroutine::detail::StackPool;
    auto* cfg = g_bbt_coroutine_config.get();
    const size_t old_preload = cfg->m_cfg_static_coroutine;
    const size_t old_limit = cfg->m_cfg_max_coroutine;
    cfg->m_cfg_static_coroutine = 3;
    cfg->m_cfg_max_coroutine = 3;

    g_bbt_profiler->Reset(); // 本模式从未启动 runtime，没有并发生产者
    g_skip_matching_new = 1;
    g_fail_new_size = sizeof(Stack); // 第一个真实 Stack 已创建；第二个对象 new 失败
    bool rejected = false;
    try { StackPool pool; }
    catch (const std::bad_alloc&) { rejected = true; }
    const bool consumed = g_fail_new_size == 0;
    g_fail_new_size = 0;
    g_skip_matching_new = 0;
    Check(rejected && consumed, "preload OOM after one real stack is observable");
    CheckProfileStackEvents(1, 1);

    g_bbt_profiler->Reset();
    StackPool::_TestFailNextPoolPush();
    {
        StackPool pool;
        Check(pool.AllocSize() == 0 && pool.GetCurCoNum() == 0,
              "failed preload publication retains no managed stack");
    }
    CheckProfileStackEvents(3, 3);

    g_bbt_profiler->Reset();
    {
        StackPool pool;
        Check(pool.AllocSize() == 3, "preload recovers after both injected failures");
    }
    CheckProfileStackEvents(3, 3); // 正常局部池析构也须为真实销毁记事件
    cfg->m_cfg_static_coroutine = old_preload;
    cfg->m_cfg_max_coroutine = old_limit;
}

int RunRejectBeforeStart()
{
    auto* cfg = g_bbt_coroutine_config.get();
    cfg->m_cfg_static_thread_num = 1;

    Check(!g_scheduler->IsInitialized(), "runtime is NOT started in reject mode");

    const size_t alloc0 = g_bbt_stackpoll->AllocSize();

    /* 显式 0 非法：invalid_argument，且在分配前拒绝 */
    bool zero_rejected = false;
    try { g_scheduler->RegistCoroutineTask(size_t(0), [](){}); }
    catch (const std::invalid_argument&) { zero_rejected = true; }
    Check(zero_rejected, "explicit 0 rejected with invalid_argument");
    Check(g_bbt_stackpoll->AllocSize() == alloc0, "no allocation on zero rejection");

    /* 取整/范围越界：length_error */
    bool range_rejected = false;
    try { g_scheduler->RegistCoroutineTask(static_cast<size_t>(-1), [](){}); }
    catch (const std::length_error&) { range_rejected = true; }
    Check(range_rejected, "out-of-range size rejected with length_error");

    bool huge_rejected = false;
    try
    {
        g_scheduler->RegistCoroutineTask(
            static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max()), [](){});
    }
    catch (const std::length_error&) { huge_rejected = true; }
    Check(huge_rejected, "PTRDIFF_MAX overflow rejected with length_error");
    Check(g_bbt_stackpoll->AllocSize() == alloc0, "no allocation on range rejections");

    /* 无 worker：注册拒绝（runtime_error），回调不执行，不留借出对象 */
    bool callback_ran = false;
    bool threw = false;
    try
    {
        g_scheduler->RegistCoroutineTask(64 * 1024, [&]() { callback_ran = true; });
    }
    catch (const std::runtime_error&) { threw = true; }
    Check(threw, "no-worker registration throws runtime_error before enqueue");
    Check(!callback_ran, "rejected task callback never executes");
    Check(g_bbt_stackpoll->GetCurCoNum() == 0, "no borrowed stack left after rejection");

    /* 失败不留对象：第二次同样失败，并复用第一次归还进缓存的栈（分配数不增） */
    const size_t alloc_after_first = g_bbt_stackpoll->AllocSize();
    Check(alloc_after_first >= alloc0, "pool accounting stays consistent after rejection");

    bool succ = true;
    g_scheduler->RegistCoroutineTask(64 * 1024, [&]() { callback_ran = true; }, succ);
    Check(!succ, "succ overload reports false without worker");
    Check(g_bbt_stackpoll->AllocSize() == alloc_after_first,
          "repeated failed registration reuses the returned stack (no new object)");
    Check(g_bbt_stackpoll->GetCurCoNum() == 0, "borrowed count returns to zero");
    Check(!callback_ran, "callback still never executed");

    RunDescriptionAssignFailure();
    RunPreloadFailures();

    std::fprintf(stdout, "[multi-size-stack] reject-before-start alloc=%zu\n",
                 g_bbt_stackpoll->AllocSize());
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr,
            "usage: %s <run-protected|run-unprotected|reject-before-start>\n", argv[0]);
        return 2;
    }

    if (std::strcmp(argv[1], "run-protected") == 0)
        RunRuntime(true);
    else if (std::strcmp(argv[1], "run-unprotected") == 0)
        RunRuntime(false);
    else if (std::strcmp(argv[1], "reject-before-start") == 0)
        RunRejectBeforeStart();
    else
    {
        std::fprintf(stderr, "unknown mode: %s\n", argv[1]);
        return 2;
    }

    std::fprintf(stdout, "[multi-size-stack] %s: %s (failures=%d)\n",
                 argv[1], g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
