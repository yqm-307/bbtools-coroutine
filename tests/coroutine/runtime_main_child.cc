// 原生子进程探针：真实宏/Run 入口、窄故障注入与可观察的执行次数。
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Stack.hpp>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <limits>
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
int g_factory_calls = 0;
const auto g_native_thread = std::this_thread::get_id();

bool Mode(const char* name) {
    return g_mode != nullptr && std::strcmp(g_mode, name) == 0;
}

void Require(bool condition) {
    if (!condition) {
        std::fprintf(stderr, "PROBE_FAIL mode=%s\n", g_mode);
        std::_Exit(90);
    }
}

void CheckReentry() {
    int factory_calls = 0;
    const auto result = RuntimeMain::RunWithOptions(+[] { return 99; }, nullptr, [&] {
        ++factory_calls;
        return RuntimeOptions{}; });
    Require(result.terminal == RuntimeMain::Terminal::StartFailed &&
            result.exit_code != 0 && factory_calls == 0);
    std::puts("REENTRY_REJECTED");
}

bbt_co_async(int) SuspendAndReturn() {
    bbtco_sleep(1);
    return 17;
}

} // namespace

// 仅测试可执行导出 interpose，不向产品库添加故障开关。
void* operator new(std::size_t size) {
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
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* ptr) noexcept {
    std::free(ptr);
}
void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}
void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}
void operator delete[](void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                              void* (*entry)(void*), void* arg) {
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

RuntimeOptions MakeOptions() {
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

void setup_runtime() {
    ++g_setup_calls;
    std::puts("SETUP");
    Require(!g_scheduler->IsInitialized() && !IsInCoroutineContext());
    g_bbt_coroutine_config->m_cfg_processer_scale_up_samples = 1;
    g_bbt_coroutine_config->m_cfg_scan_interval_ms = 7;
}

int bbtco_main_thread_hook() {
    ++g_hook_calls;
    std::puts("HOOK");
    Require(g_scheduler->IsInitialized() && !IsInCoroutineContext() &&
            std::this_thread::get_id() == g_native_thread && g_main_calls == 0);
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
        // hook 返回后，下一次 native 分配属于真实主任务提交路径。
        g_fail_new = true;
    }
    if (Mode("submit_stack_failed")) {
        detail::Stack::_TestFailNextAlloc();
    }
    if (g_setup_calls != 0)
        g_bbt_coroutine_config->m_cfg_scan_interval_ms = 999;
    return 0;
}

int bbtco_main() {
    ++g_main_calls;
    std::puts("MAIN");
    Require(IsInCoroutineContext() && g_scheduler->IsInitialized());
    if (g_setup_calls != 0)
        Require(g_bbt_coroutine_config->m_cfg_processer_scale_up_samples == 1 &&
                g_bbt_coroutine_config->IsRuntimeSetupFrozen() &&
                g_bbt_coroutine_config->RuntimeSnapshot().scan_interval_ms == 7);
    if (g_factory_calls != 0) {
        const auto expected = detail::Stack::NormalizeSize(64 * 1024 + 1);
        Require(GetLocalCoroutineStackSize() == expected && g_scheduler->GetProcesserNum() == 1);
        Require(g_bbt_coroutine_config->m_cfg_stack_protect == !Mode("unprotected"));
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
        bbtco[entered, never] {
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
#else
int main() {
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
    const bool use_default = Mode("default") || Mode("record_oom") || Mode("already_started");
    const auto result = use_default
                            ? RuntimeMain::Run(&bbtco_main)
                            : RuntimeMain::RunWithOptions(Mode("null_task") ? nullptr : &bbtco_main,
                                                          &bbtco_main_thread_hook, MakeOptions);
    if (result.terminal == RuntimeMain::Terminal::StartFailed && !use_default) {
        Require(!g_scheduler->IsInitialized());
        if (!Mode("start_failed")) {
            Require(cfg.m_cfg_static_thread_num == old_workers && cfg.m_cfg_stack_size == old_stack &&
                    cfg.m_cfg_stack_protect == old_protect);
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
    std::printf("DONE terminal=%d code=%d main=%d hook=%d factory=%d new_hits=%d thread_hits=%d\n",
                static_cast<int>(result.terminal), result.exit_code, g_main_calls.load(),
                g_hook_calls.load(), g_factory_calls, g_new_hits.load(), g_thread_hits.load());
    return result.exit_code;
}
#endif
