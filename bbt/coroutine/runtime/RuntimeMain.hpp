/**
 * @file RuntimeMain.hpp
 * @brief runtime 拥有的 native main 薄入口及主任务完成结果。
 */
#pragma once
#include <bbt/coroutine/runtime/RuntimeOptions.hpp>
#include <functional>
#include <memory>

namespace bbt::coroutine {

namespace detail {
class HostHookState;
}

/**
 * @brief 提交后 host hook 的运行时控制面：runtime 持有，hook 只登记与观察。
 *
 * 登记为单槽：后一次登记替换尚未被取出的前一次。主任务完成时（含未捕获
 * 异常）runtime 恰好发起一次已登记的调用；登记时主任务已完成则立即在登记
 * 线程回调。登记决定与完成发布在同一临界区内提交，谁先进入谁决定槽位归属：
 * 完成发布先提交时，其后到来的登记一律按迟到登记回调，完成路径不会再执行
 * 一次已被替换的旧登记。回调在 runtime 内部锁之外执行，不得抛异常、不得阻塞。
 *
 * 登记寿命只到 host hook 返回或抛异常：此后 runtime 在返回路径丢弃未取出
 * 的登记、不再发起新回调。与完成路径竞争时已被取出的回调仍可能继续执行，
 * 清除登记无法撤销在途回调；闭包必须以值或自有共享状态捕获，runtime 不
 * 保护任意 hook 栈引用。主任务完成记录在已发起的回调返回后才发布，因此
 * RuntimeMain 返回后不会再执行本登记的回调。
 */
class HostHookContext {
public:
    void OnMainTaskDone(std::function<void()> callback) noexcept;
    bool MainTaskDone() const noexcept;

private:
    friend class RuntimeMain;
    explicit HostHookContext(std::shared_ptr<detail::HostHookState> state);

    std::shared_ptr<detail::HostHookState> m_state;
};

/**
 * @brief 提交后 host hook：在原生 main 线程调用一次；允许宿主 native 阻塞入口，
 *  但不得调用 Co 原语、不得提交主任务。
 */
using HostHook = int (*)(HostHookContext&);

/**
 * @brief 单次启动、执行 setup_runtime、提交前 native hook、提交主协程，并等待主任务
 * （以及可选的提交后 host hook）结束。
 *
 * 运行时是进程寿命对象；本入口不停止 scheduler、不排空 detached 任务。
 * setup_runtime 是 Start 前配置全局 GlobalConfig 的唯一生命周期阶段；
 * 业务资源收口必须在主任务返回前完成。重复或在受管执行域中调用返回
 * StartFailed，不再启动或等待。
 */
class RuntimeMain {
public:
    /**
     * @brief 主入口唯一完成结果的原因。
     */
    enum class Terminal {
        MainReturned,
        HookRejected,
        HookThrew,
        SetupThrew,
        StartFailed,
        SubmitFailed,
        MainThrew,
        HostHookFailed,
        HostHookThrew,
    };

    /**
     * @brief 原因与 native main 返回值；异常/启动/提交失败的退出码为 1。
     */
    struct Completion {
        Terminal terminal;
        int exit_code;
    };

    /**
     * @brief 兼容无 setup_runtime 的旧入口。
     */
    static Completion Run(int (*main_task)(), int (*native_hook)() = nullptr,
                          const RuntimeOptions& options = {}) noexcept;

    /**
     * @brief 执行 setup_runtime 后固定执行 options → Start → hook → submit。
     * setup_runtime 在 native 线程、Start 前执行，可以设置 GlobalConfig 的全部启动字段。
     */
    static Completion RunWithSetup(void (*setup_runtime)(), int (*main_task)(), int (*native_hook)() = nullptr,
                                   const RuntimeOptions& options = {}) noexcept;

    /**
     * @brief 提交成功后，在原生 main 线程执行一次提交后 host hook。
     */
    static Completion RunWithSetupAndHostHook(void (*setup_runtime)(), int (*main_task)(), HostHook host_hook,
                                              const RuntimeOptions& options = {}) noexcept;

    /**
     * @brief 在同一异常边界内只求值一次 RuntimeOptions 工厂。
     */
    template <class OptionsFactory>
    static Completion RunWithOptions(int (*main_task)(), int (*native_hook)(), OptionsFactory&& make_options) noexcept;

    /**
     * @brief setup_runtime 后在同一异常边界内只求值一次 RuntimeOptions 工厂。
     */
    template <class OptionsFactory>
    static Completion RunWithSetupAndOptions(void (*setup_runtime)(), int (*main_task)(), int (*native_hook)(),
                                             OptionsFactory&& make_options) noexcept;

    /**
     * @brief 提交后 host hook 与 RuntimeOptions 工厂共用同一 runtime 异常边界。
     */
    template <class OptionsFactory>
    static Completion RunWithSetupHostHookAndOptions(void (*setup_runtime)(), int (*main_task)(), HostHook host_hook,
                                                     OptionsFactory&& make_options) noexcept;

private:
    static Completion RunWithFactory(void (*setup_runtime)(), int (*main_task)(), int (*native_hook)(),
                                     HostHook host_hook, const void* factory,
                                     RuntimeOptions (*evaluate)(const void*)) noexcept;
};

}

#include <bbt/coroutine/runtime/__TRuntimeMain.hpp>
