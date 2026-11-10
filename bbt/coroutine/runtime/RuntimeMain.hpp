/** @file RuntimeMain.hpp
 * @brief runtime 拥有的 native main 薄入口及主任务完成结果。
 */
#pragma once
#include <bbt/coroutine/runtime/RuntimeOptions.hpp>

namespace bbt::coroutine {

/**
 * @brief 单次启动、执行 setup_runtime、原生 hook、提交主协程，并仅等待该主任务。
 *
 * 运行时是进程寿命对象；本入口不停止 scheduler、不排空 detached 任务。
 * setup_runtime 是 Start 前配置全局 GlobalConfig 的唯一生命周期阶段；
 * 业务资源收口必须在主任务返回前完成。重复或在受管执行域中调用返回
 * StartFailed，不再启动或等待。
 */
class RuntimeMain {
public:
    /** @brief 主入口唯一完成结果的原因。 */
    enum class Terminal {
        MainReturned,
        HookRejected,
        HookThrew,
        SetupThrew,
        StartFailed,
        SubmitFailed,
        MainThrew,
    };

    /** @brief 原因与 native main 返回值；异常/启动/提交失败的退出码为 1。 */
    struct Completion {
        Terminal terminal;
        int exit_code;
    };

    /** @brief 兼容无 setup_runtime 的旧入口。 */
    static Completion Run(int (*main_task)(), int (*native_hook)() = nullptr,
                          const RuntimeOptions& options = {}) noexcept;

    /**
     * @brief 执行 setup_runtime 后固定执行 options → Start → hook → submit。
     * setup_runtime 在 native 线程、Start 前执行，可以设置 GlobalConfig 的全部启动字段。
     */
    static Completion RunWithSetup(void (*setup_runtime)(), int (*main_task)(),
                                   int (*native_hook)() = nullptr,
                                   const RuntimeOptions& options = {}) noexcept;

    /** @brief 在同一异常边界内只求值一次 RuntimeOptions 工厂。 */
    template <class OptionsFactory>
    static Completion RunWithOptions(int (*main_task)(), int (*native_hook)(),
                                     OptionsFactory&& make_options) noexcept;

    /** @brief setup_runtime 后在同一异常边界内只求值一次 RuntimeOptions 工厂。 */
    template <class OptionsFactory>
    static Completion RunWithSetupAndOptions(void (*setup_runtime)(), int (*main_task)(),
                                             int (*native_hook)(),
                                             OptionsFactory&& make_options) noexcept;

private:
    static Completion RunWithFactory(void (*setup_runtime)(), int (*main_task)(),
                                     int (*native_hook)(), const void* factory,
                                     RuntimeOptions (*evaluate)(const void*)) noexcept;
};

} // namespace bbt::coroutine

#include <bbt/coroutine/runtime/RuntimeMainImpl.hpp>
