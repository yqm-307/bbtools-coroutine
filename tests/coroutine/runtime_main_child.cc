/**
 * 原生子进程探针：真实宏/Run 入口、窄故障注入与可观察的执行次数。
 */
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Stack.hpp>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <memory>
#include <new>
#include <pthread.h>
#include <stdexcept>
#include <thread>

using namespace bbt::coroutine;
namespace {
const char* g_mode = std::getenv("BBT_ENTRY_CASE");
thread_local bool g_fail_new = false;
std::atomic_bool g_fail_thread{false};
std::atomic<int> g_new_hits{0};
std::atomic<int> g_thread_hits{0};
std::atomic<int> g_main_calls{0};
std::atomic<int> g_hook_calls{0};
std::atomic<int> g_setup_calls{0};
std::atomic<int> g_host_calls{0};
int g_factory_calls = 0;
const auto g_native_thread = std::this_thread::get_id();

/**
 * host hook 用例的屏障只用原子位与 yield：不靠 sleep 制造窗口，也不等待超时。
 */
std::atomic_bool g_published{false};
std::atomic_bool g_host_running{false};
std::atomic_bool g_close_requested{false};
std::atomic_bool g_hook_returned{false};
std::atomic_bool g_hook_leaving{false};
std::atomic_bool g_release_callback{false};
std::atomic_bool g_hook_local_destroyed{false};
std::atomic_bool g_host_failed{false};
std::atomic_bool g_callback_registered{false};
std::atomic_bool g_host_threw{false};
std::atomic_bool g_host_registered{false};
std::atomic<int> g_arrivals{0};

struct HostProbe {
    std::atomic<int> calls{0};

    /**
     * 被后一次登记替换掉的旧登记执行次数
     */
    std::atomic<int> replaced{0};

    /**
     * 回调内重入登记的执行次数
     */
    std::atomic<int> reentered{0};
    std::atomic<int> inline_native{0};
    std::atomic_bool in_flight{false};
    std::atomic_bool done{false};
    std::atomic_bool in_flight_at_hook_exit{false};
};
std::shared_ptr<HostProbe> g_host_probe;

struct HookLocal {
    ~HookLocal() { g_hook_local_destroyed.store(true, std::memory_order_release); }
};

bool Mode(const char* name)
{
    return g_mode != nullptr && std::strcmp(g_mode, name) == 0;
}

bool HostMode()
{
    return g_mode != nullptr && std::strncmp(g_mode, "host_", 5) == 0;
}

void Require(bool condition)
{
    if (!condition) {
        std::fprintf(stderr, "PROBE_FAIL mode=%s\n", g_mode);
        std::_Exit(90);
    }
}

void WaitFlag(const std::atomic_bool& flag)
{
    while (!flag.load(std::memory_order_acquire))
        std::this_thread::yield();
}

void CheckReentry()
{
    int factory_calls = 0;
    const auto result = RuntimeMain::RunWithOptions(
        +[] { return 99; }, nullptr,
        [&] {
            ++factory_calls;
            return RuntimeOptions{};
        });
    Require(result.terminal == RuntimeMain::Terminal::StartFailed && result.exit_code != 0 && factory_calls == 0);
    std::puts("REENTRY_REJECTED");
}

bbt_co_async(int) SuspendAndReturn()
{
    bbtco_sleep(1);
    return 17;
}

int RunHostMain()
{
    if (Mode("host_publish")) {
        g_published.store(true, std::memory_order_release);
        WaitFlag(g_host_running);
        std::puts("MAIN_CLOSE");
        g_close_requested.store(true, std::memory_order_release);
        std::puts("MAIN_RETURN");
        return 37;
    }
    if (Mode("host_main_done")) {
        Require((bbt_co_await SuspendAndReturn()) == 17);
        std::puts("MAIN_RETURN");
        return 21;
    }
    if (Mode("host_main_threw")) {
        throw std::runtime_error("host active main failure");
    }
    if (Mode("host_late_register")) {
        std::puts("MAIN_RETURN");
        return 37;
    }
    if (Mode("host_register_race")) {
        g_arrivals.fetch_add(1, std::memory_order_acq_rel);
        while (g_arrivals.load(std::memory_order_acquire) < 2)
            std::this_thread::yield();
        std::puts("MAIN_RETURN");
        return 37;
    }
    if (Mode("host_replace")) {
        /**
         * 两次登记都在本返回之前提交：替换一定先于完成发布线性化。
         */
        WaitFlag(g_host_registered);
        std::puts("MAIN_RETURN");
        return 37;
    }
    if (Mode("host_replace_race")) {
        g_arrivals.fetch_add(1, std::memory_order_acq_rel);
        while (g_arrivals.load(std::memory_order_acquire) < 2)
            std::this_thread::yield();
        std::puts("MAIN_RETURN");
        return 37;
    }
    if (Mode("host_callback_reentry")) {
        std::puts("MAIN_RETURN");
        return 37;
    }
    if (Mode("host_exits_first")) {
        WaitFlag(g_hook_returned);
        std::puts("MAIN_RETURN");
        return 37;
    }
    if (Mode("host_callback_in_flight")) {
        /**
         * 等登记完成再返回：让完成路径取走已登记的槽，而不是在登记调用内联执行。
         */
        WaitFlag(g_callback_registered);
        std::puts("MAIN_RETURN");
        return 37;
    }
    if (Mode("host_failed")) {
        WaitFlag(g_host_failed);
        std::puts("MAIN_ABORT");
        return 99;
    }
    if (Mode("host_threw")) {
        WaitFlag(g_host_threw);
        std::puts("MAIN_ABORT");
        return 37;
    }
    Require((bbt_co_await SuspendAndReturn()) == 17);
    std::puts("MAIN_RETURN");
    return 37;
}

[[maybe_unused]] void CheckHostOutcome()
{
    const auto probe = g_host_probe;
    if (Mode("host_publish")) {
        Require(g_published.load() && g_host_running.load() && g_close_requested.load());
    } else if (Mode("host_main_done") || Mode("host_main_threw")) {
        Require(g_close_requested.load() && probe && probe->calls.load() == 1);
    } else if (Mode("host_late_register")) {
        Require(probe && probe->calls.load() == 1 && probe->inline_native.load() == 1);
    } else if (Mode("host_register_race")) {
        Require(probe && probe->calls.load() == 1);
    } else if (Mode("host_replace")) {
        /**
         * 替换先于完成发布：旧登记不得执行，后登记恰好一次。
         */
        Require(probe && probe->calls.load() == 1 && probe->replaced.load() == 0);
    } else if (Mode("host_replace_race")) {
        /**
         * 替换与完成发布并发：后登记恰好一次，被替换的旧登记至多一次。
         */
        Require(probe && probe->calls.load() == 1 && probe->replaced.load() <= 1);
    } else if (Mode("host_callback_reentry")) {
        /**
         * 回调在锁外执行：回调内查询完成位与再次登记都不自我死锁，嵌套登记立即执行。
         */
        Require(probe && probe->calls.load() == 1 && probe->reentered.load() == 1);
    } else if (Mode("host_exits_first")) {
        /**
         * 登记在 hook 返回前发生、完成在之后发生；是否已在返回前被取出属竞争面，
         * 这里只要求不重复、不崩溃，完成记录由 DONE 的恰好一条证明。
         */
        Require(probe && probe->calls.load() <= 1);
    } else if (Mode("host_callback_in_flight")) {
        Require(probe && probe->calls.load() == 1 && probe->in_flight_at_hook_exit.load() && probe->done.load()
                && g_hook_local_destroyed.load());
    } else if (Mode("host_failed")) {
        Require(g_host_failed.load());
    } else if (Mode("host_threw")) {
        Require(g_host_threw.load() && probe && probe->calls.load() <= 1);
    }
}

}

/**
 * 仅测试可执行导出 interpose，不向产品库添加故障开关。
 */
void* operator new(std::size_t size)
{
    if (g_fail_new) {
        g_fail_new = false;
        ++g_new_hits;
        throw std::bad_alloc();
    }
    if (auto* result = std::malloc(size == 0 ? 1 : size)) {
        return result;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size)
{
    return ::operator new(size);
}
void operator delete(void* ptr) noexcept
{
    std::free(ptr);
}
void operator delete[](void* ptr) noexcept
{
    std::free(ptr);
}
void operator delete(void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}
void operator delete[](void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*entry)(void*), void* arg)
{
    if (g_fail_thread.exchange(false)) {
        ++g_thread_hits;
        return EAGAIN;
    }
    using Create = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
    static const auto create = reinterpret_cast<Create>(dlsym(RTLD_NEXT, "pthread_create"));
    if (create == nullptr) {
        std::_Exit(91);
    }
    return create(thread, attr, entry, arg);
}

RuntimeOptions MakeOptions()
{
    ++g_factory_calls;
    std::puts("FACTORY");
    Require(!g_scheduler->IsInitialized() && !IsInCoroutineContext());
    if (Mode("factory_threw")) {
        throw 7;
    }
    RuntimeOptions options;
    options.initial_workers = 1;
    options.default_stack = StackRequest{64 * 1024 + 1, !Mode("unprotected")};
    if (Mode("zero_workers")) {
        options.initial_workers = 0;
    } else if (Mode("huge_workers")) {
        options.initial_workers = std::numeric_limits<std::size_t>::max();
    } else if (Mode("zero_stack")) {
        options.default_stack->usable_bytes = 0;
    } else if (Mode("overflow_stack")) {
        options.default_stack->usable_bytes = std::numeric_limits<std::size_t>::max();
    } else if (Mode("strict") || Mode("invalid_budget")) {
        options.stacks = StackConfig{StackPolicy::Strict, {{{64 * 1024 + 1, true}, 16}}, 0, 0};
        if (Mode("invalid_budget")) {
            options.stacks->strict_classes[0].max_objects = 0;
        }
    } else if (Mode("auto")) {
        options.stacks = StackConfig{StackPolicy::Auto, {}, 64, 16 * 1024 * 1024};
    } else if (Mode("bounds")) {
        g_bbt_coroutine_config->m_cfg_processer_min_num = 2;
    }
    return options;
}

void setup_runtime()
{
    ++g_setup_calls;
    std::puts("SETUP");
    Require(!g_scheduler->IsInitialized() && !IsInCoroutineContext());
    g_bbt_coroutine_config->m_cfg_processer_scale_up_samples = 1;
    g_bbt_coroutine_config->m_cfg_scan_interval_ms = 7;
}

int bbtco_main_thread_hook()
{
    ++g_hook_calls;
    std::puts("HOOK");
    Require(g_scheduler->IsInitialized() && !IsInCoroutineContext() && std::this_thread::get_id() == g_native_thread
            && g_main_calls == 0);
    if (Mode("hook_rejected")) {
        return 23;
    }
    if (Mode("hook_threw")) {
        throw 42;
    }
    if (Mode("hook_reentry")) {
        CheckReentry();
    }
    if (Mode("submit_failed")) {
        /**
         * hook 返回后，下一次 native 分配属于真实主任务提交路径。
         */
        g_fail_new = true;
    }
    if (Mode("submit_stack_failed")) {
        detail::Stack::_TestFailNextAlloc();
    }
    if (g_setup_calls != 0)
        g_bbt_coroutine_config->m_cfg_scan_interval_ms = 999;
    return 0;
}

/**
 * 提交后 host hook：模拟 framework 在原生 main 线程调用一次 Engine::Run。
 */
int bbtco_main_host_hook(HostHookContext& ctx)
{
    ++g_host_calls;
    std::puts("HOST_ENTER");
    Require(g_scheduler->IsInitialized() && !IsInCoroutineContext() && std::this_thread::get_id() == g_native_thread);

    if (Mode("host_publish")) {
        /**
         * 等主任务发布 Ready：若 runtime 在提交前执行本 hook，这里永远等不到。
         */
        WaitFlag(g_published);
        Require(g_main_calls.load() == 1);
        g_host_running.store(true, std::memory_order_release);
        WaitFlag(g_close_requested);
        std::puts("HOST_RETURN");
        return 0;
    }
    if (Mode("host_main_done") || Mode("host_main_threw")) {
        auto probe = std::make_shared<HostProbe>();
        g_host_probe = probe;
        ctx.OnMainTaskDone([probe] {
            probe->calls.fetch_add(1, std::memory_order_acq_rel);
            g_close_requested.store(true, std::memory_order_release);
        });
        WaitFlag(g_close_requested);
        Require(probe->calls.load() == 1);
        std::puts("HOST_CALLBACK_SEEN");
        return 0;
    }
    if (Mode("host_late_register")) {
        auto probe = std::make_shared<HostProbe>();
        g_host_probe = probe;
        while (!ctx.MainTaskDone())
            std::this_thread::yield();
        probe->inline_native.store(std::this_thread::get_id() == g_native_thread ? 1 : 0, std::memory_order_release);
        ctx.OnMainTaskDone([probe] { probe->calls.fetch_add(1, std::memory_order_acq_rel); });

        /**
         * 已完成时立即在登记线程回调：本语句之后即可观察到唯一一次。
         */
        Require(probe->calls.load() == 1);
        std::puts("HOST_LATE_REGISTER");
        return 0;
    }
    if (Mode("host_register_race")) {
        auto probe = std::make_shared<HostProbe>();
        g_host_probe = probe;
        g_arrivals.fetch_add(1, std::memory_order_acq_rel);
        while (g_arrivals.load(std::memory_order_acquire) < 2)
            std::this_thread::yield();
        ctx.OnMainTaskDone([probe] { probe->calls.fetch_add(1, std::memory_order_acq_rel); });

        /**
         * 等完成发布落地再返回：否则返回路径可能先丢弃未取出的登记，把"登记与
         * 完成并发"退化成"登记被丢弃"，执行次数就依赖调度顺序。
         */
        while (!ctx.MainTaskDone())
            std::this_thread::yield();
        std::puts("HOST_REGISTERED");
        return 0;
    }
    if (Mode("host_replace")) {
        auto probe = std::make_shared<HostProbe>();
        g_host_probe = probe;
        ctx.OnMainTaskDone([probe] {
            std::puts("STALE_RAN");
            probe->replaced.fetch_add(1, std::memory_order_acq_rel);
        });

        /**
         * 第二次登记替换尚未被取走的第一次：完成路径只能执行后一次。
         */
        ctx.OnMainTaskDone([probe] {
            probe->calls.fetch_add(1, std::memory_order_acq_rel);
            g_close_requested.store(true, std::memory_order_release);
        });
        g_host_registered.store(true, std::memory_order_release);

        /**
         * 等后一次登记被执行后才返回：返回路径会丢弃未取出的登记。
         */
        WaitFlag(g_close_requested);
        std::puts("HOST_REPLACED");
        return 0;
    }
    if (Mode("host_replace_race")) {
        auto probe = std::make_shared<HostProbe>();
        g_host_probe = probe;
        ctx.OnMainTaskDone([probe] {
            std::puts("STALE_RAN");
            probe->replaced.fetch_add(1, std::memory_order_acq_rel);
        });
        g_arrivals.fetch_add(1, std::memory_order_acq_rel);
        while (g_arrivals.load(std::memory_order_acquire) < 2)
            std::this_thread::yield();

        /**
         * 与完成发布并发登记：后登记要么替换旧槽（旧登记不执行），要么按迟到
         * 登记内联执行；两条路径下它都必须恰好执行一次。
         */
        ctx.OnMainTaskDone([probe] {
            probe->calls.fetch_add(1, std::memory_order_acq_rel);
            g_close_requested.store(true, std::memory_order_release);
        });
        WaitFlag(g_close_requested);
        std::puts("HOST_REPLACED");
        return 0;
    }
    if (Mode("host_callback_reentry")) {
        auto probe = std::make_shared<HostProbe>();
        g_host_probe = probe;
        while (!ctx.MainTaskDone())
            std::this_thread::yield();

        /**
         * 迟到登记在本线程同步执行：借用 hook 栈上的 ctx 只在本次调用内有效。
         */
        ctx.OnMainTaskDone([probe, &ctx] {
            Require(ctx.MainTaskDone());
            ctx.OnMainTaskDone([probe] { probe->calls.fetch_add(1, std::memory_order_acq_rel); });
            probe->reentered.fetch_add(1, std::memory_order_acq_rel);
        });
        Require(probe->reentered.load() == 1 && probe->calls.load() == 1);
        std::puts("HOST_REENTRY");
        return 0;
    }
    if (Mode("host_exits_first")) {
        auto probe = std::make_shared<HostProbe>();
        g_host_probe = probe;

        /**
         * hook 内重入不会启动第二次 runtime。
         */
        CheckReentry();
        ctx.OnMainTaskDone([probe] { probe->calls.fetch_add(1, std::memory_order_acq_rel); });

        /**
         * 先打点再放行主任务：主任务完成一定发生在本 hook 返回路径之内。
         */
        std::puts("HOST_RETURN");
        g_hook_returned.store(true, std::memory_order_release);
        return 0;
    }
    if (Mode("host_callback_in_flight")) {
        auto probe = std::make_shared<HostProbe>();
        g_host_probe = probe;

        /**
         * hook 栈上的对象只属于本函数；回调不引用它，而用自有共享状态。
         */
        HookLocal local;
        std::thread([probe] {
            WaitFlag(g_hook_leaving);
            WaitFlag(probe->in_flight);
            g_release_callback.store(true, std::memory_order_release);
        }).detach();
        ctx.OnMainTaskDone([probe] {
            probe->calls.fetch_add(1, std::memory_order_acq_rel);
            probe->in_flight.store(true, std::memory_order_release);
            WaitFlag(g_release_callback);
            probe->done.store(true, std::memory_order_release);
        });
        g_callback_registered.store(true, std::memory_order_release);

        /**
         * 在返回前确认回调已经执行且尚未完成：返回即“在途回调与 hook 退出”交错。
         */
        WaitFlag(probe->in_flight);
        Require(!probe->done.load());
        probe->in_flight_at_hook_exit.store(true, std::memory_order_release);
        g_hook_leaving.store(true, std::memory_order_release);
        std::puts("HOST_RETURN");
        return 0;
    }
    if (Mode("host_failed")) {
        g_host_failed.store(true, std::memory_order_release);
        std::puts("HOST_FAIL");
        return 23;
    }
    if (Mode("host_threw")) {
        auto probe = std::make_shared<HostProbe>();
        g_host_probe = probe;
        ctx.OnMainTaskDone([probe] { probe->calls.fetch_add(1, std::memory_order_acq_rel); });
        g_host_threw.store(true, std::memory_order_release);
        std::puts("HOST_THROW");
        throw 42;
    }
    std::puts("HOST_RETURN");
    return 0;
}

int bbtco_main()
{
    ++g_main_calls;
    std::puts("MAIN");
    Require(IsInCoroutineContext() && g_scheduler->IsInitialized());
    if (g_setup_calls != 0)
        Require(g_bbt_coroutine_config->m_cfg_processer_scale_up_samples == 1
                && g_bbt_coroutine_config->IsRuntimeSetupFrozen()
                && g_bbt_coroutine_config->RuntimeSnapshot().scan_interval_ms == 7);
    if (g_factory_calls != 0) {
        const auto expected = detail::Stack::NormalizeSize(64 * 1024 + 1);
        Require(GetLocalCoroutineStackSize() == expected && g_scheduler->GetProcesserNum() == 1);
        Require(g_bbt_coroutine_config->m_cfg_stack_protect == !Mode("unprotected"));
    }
    if (HostMode()) {
        return RunHostMain();
    }
    if (Mode("main_threw")) {
        throw std::runtime_error("main failure");
    }
    if (Mode("main_threw_nonstd")) {
        throw 3;
    }
    if (Mode("main_reentry")) {
        CheckReentry();
    }
    if (Mode("detached")) {
        auto entered = Chan<int, 1>();
        auto never = Chan<int, 1>();
        bbtco[entered, never]
        {
            (*entered) << 1;
            int value = 0;
            (*never) >> value;
            std::puts("DETACHED_FINISHED");
        };
        int value = 0;
        (*entered) >> value;
        Require(value == 1);
        std::puts("DETACHED_ENTERED");
    }
    Require((bbt_co_await SuspendAndReturn()) == 17);
    std::puts("MAIN_RETURN");
    return Mode("zero") ? 0 : 37;
}

#if BBT_ENTRY_VARIANT == 1
BBTCO_MAIN()
#elif BBT_ENTRY_VARIANT == 2
BBTCO_MAIN_WITH_HOOK()
#elif BBT_ENTRY_VARIANT == 3
BBTCO_MAIN_WITH_OPTIONS(MakeOptions())
#elif BBT_ENTRY_VARIANT == 4
BBTCO_MAIN_WITH_HOOK_AND_OPTIONS(MakeOptions())
#elif BBT_ENTRY_VARIANT == 5
BBTCO_MAIN_WITH_SETUP_HOOK_AND_OPTIONS(MakeOptions())
#elif BBT_ENTRY_VARIANT == 6
BBTCO_MAIN_WITH_SETUP_AND_HOST_HOOK()
#elif BBT_ENTRY_VARIANT == 7
BBTCO_MAIN_WITH_SETUP_HOST_HOOK_AND_OPTIONS(MakeOptions())
#else
int main()
{
    Require(g_mode != nullptr);
    auto& cfg = *g_bbt_coroutine_config;
    const auto old_workers = cfg.m_cfg_static_thread_num;
    const auto old_stack = cfg.m_cfg_stack_size;
    const auto old_protect = cfg.m_cfg_stack_protect;
    if (Mode("start_failed")) {
        g_fail_thread = true;
    } else if (Mode("record_oom")) {
        g_fail_new = true;
    } else if (Mode("already_started")) {
        cfg.m_cfg_static_thread_num = 1;
        g_scheduler->Start();
    }
    const bool host_mode = HostMode();
    const bool use_default = Mode("default") || Mode("record_oom") || Mode("already_started");
    const auto result = host_mode
                            ? RuntimeMain::RunWithSetupHostHookAndOptions(&setup_runtime, &bbtco_main,
                                                                          &bbtco_main_host_hook, MakeOptions)
                            : (use_default ? RuntimeMain::Run(&bbtco_main)
                                           : RuntimeMain::RunWithOptions(Mode("null_task") ? nullptr : &bbtco_main,
                                                                         &bbtco_main_thread_hook, MakeOptions));
    if (result.terminal == RuntimeMain::Terminal::StartFailed && !use_default && !host_mode) {
        Require(!g_scheduler->IsInitialized());
        if (!Mode("start_failed")) {
            Require(cfg.m_cfg_static_thread_num == old_workers && cfg.m_cfg_stack_size == old_stack
                    && cfg.m_cfg_stack_protect == old_protect);
        }
    }
    if (Mode("start_failed")) {
        Require(g_thread_hits == 1);
    }
    if (Mode("submit_failed") || Mode("record_oom")) {
        Require(g_new_hits == 1);
    }
    if (Mode("repeat")) {
        CheckReentry();
    }
    if (host_mode) {
        CheckHostOutcome();
    }
    const int callbacks = g_host_probe ? g_host_probe->calls.load() : 0;
    std::printf("DONE terminal=%d code=%d main=%d hook=%d factory=%d new_hits=%d thread_hits=%d "
                "host=%d cb=%d\n",
                static_cast<int>(result.terminal), result.exit_code, g_main_calls.load(), g_hook_calls.load(),
                g_factory_calls, g_new_hits.load(), g_thread_hits.load(), g_host_calls.load(), callbacks);
    return result.exit_code;
}
#endif
