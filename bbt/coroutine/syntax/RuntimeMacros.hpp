/**
 * @file RuntimeMacros.hpp
 * @brief Co-only 应用入口宏；实现位于 runtime 子模块。
 */
#pragma once

#include <bbt/coroutine/runtime/RuntimeMain.hpp>

/**
 * @brief 无额外全局配置阶段的应用入口。
 */
#define BBTCO_MAIN()                                                       \
    int main() {                                                           \
        return ::bbt::coroutine::RuntimeMain::Run(&bbtco_main).exit_code;  \
    }

/**
 * @brief 无额外全局配置阶段，并调用 int bbtco_main_thread_hook()。
 */
#define BBTCO_MAIN_WITH_HOOK()                                                        \
    int main() {                                                                      \
        return ::bbt::coroutine::RuntimeMain::Run(&bbtco_main, &bbtco_main_thread_hook).exit_code; \
    }

/**
 * @brief 无额外全局配置阶段，配置表达式在 runtime 异常边界内求值。
 */
#define BBTCO_MAIN_WITH_OPTIONS(options_expr)                                    \
    int main() {                                                                  \
        return ::bbt::coroutine::RuntimeMain::RunWithOptions(                     \
                   &bbtco_main, nullptr, [] { return (options_expr); })           \
            .exit_code;                                                           \
    }

/**
 * @brief 无额外全局配置阶段，并调用 hook 与配置表达式。
 */
#define BBTCO_MAIN_WITH_HOOK_AND_OPTIONS(options_expr)                             \
    int main() {                                                                   \
        return ::bbt::coroutine::RuntimeMain::RunWithOptions(                      \
                   &bbtco_main, &bbtco_main_thread_hook, [] { return (options_expr); }) \
            .exit_code;                                                            \
    }

/**
 * @brief 定义 setup_runtime()，在 Start 前钉死既有 GlobalConfig 后启动应用。
 */
#define BBTCO_MAIN_WITH_SETUP()                                                    \
    int main() {                                                                   \
        return ::bbt::coroutine::RuntimeMain::RunWithSetup(                        \
                   &setup_runtime, &bbtco_main).exit_code;                         \
    }

/**
 * @brief setup_runtime() 后调用 hook。
 */
#define BBTCO_MAIN_WITH_SETUP_AND_HOOK()                                           \
    int main() {                                                                   \
        return ::bbt::coroutine::RuntimeMain::RunWithSetup(                        \
                   &setup_runtime, &bbtco_main, &bbtco_main_thread_hook).exit_code; \
    }

/**
 * @brief setup_runtime() 后应用 RuntimeOptions。
 */
#define BBTCO_MAIN_WITH_SETUP_AND_OPTIONS(options_expr)                            \
    int main() {                                                                   \
        return ::bbt::coroutine::RuntimeMain::RunWithSetupAndOptions(               \
                   &setup_runtime, &bbtco_main, nullptr, [] { return (options_expr); }) \
            .exit_code;                                                            \
    }

/**
 * @brief setup_runtime() 后应用 RuntimeOptions 并调用 hook。
 */
#define BBTCO_MAIN_WITH_SETUP_HOOK_AND_OPTIONS(options_expr)                       \
    int main() {                                                                   \
        return ::bbt::coroutine::RuntimeMain::RunWithSetupAndOptions(               \
                   &setup_runtime, &bbtco_main, &bbtco_main_thread_hook,            \
                   [] { return (options_expr); })                                  \
            .exit_code;                                                            \
    }

/**
 * @brief setup_runtime() 后提交主任务，并在原生 main 线程调用 bbtco_main_host_hook。
 */
#define BBTCO_MAIN_WITH_SETUP_AND_HOST_HOOK()                                      \
    int main() {                                                                   \
        return ::bbt::coroutine::RuntimeMain::RunWithSetupAndHostHook(              \
                   &setup_runtime, &bbtco_main, &bbtco_main_host_hook)              \
            .exit_code;                                                            \
    }

/**
 * @brief setup_runtime() 后应用 RuntimeOptions，再调用提交后 host hook。
 */
#define BBTCO_MAIN_WITH_SETUP_HOST_HOOK_AND_OPTIONS(options_expr)                  \
    int main() {                                                                   \
        return ::bbt::coroutine::RuntimeMain::RunWithSetupHostHookAndOptions(       \
                   &setup_runtime, &bbtco_main, &bbtco_main_host_hook,              \
                   [] { return (options_expr); })                                  \
            .exit_code;                                                            \
    }
