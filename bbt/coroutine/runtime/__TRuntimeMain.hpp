/**
 * @file __TRuntimeMain.hpp
 * @brief RuntimeMain 的模板实现：配置值工厂的同步转发，由 RuntimeMain.hpp 末尾包含。
 */
#pragma once
#include <bbt/coroutine/runtime/RuntimeMain.hpp>

namespace bbt::coroutine {

template <class OptionsFactory>
RuntimeMain::Completion RuntimeMain::RunWithOptions(int (*main_task)(), int (*native_hook)(),
                                                    OptionsFactory&& make_options) noexcept
{

    /**
     * 借用 callable（也支持函数左值/不可复制工厂），不分配或复制配置。
     */
    const auto evaluate = [&make_options] {
        return make_options();
    };
    return RunWithFactory(nullptr, main_task, native_hook, nullptr, &evaluate,
                          [](const void* factory) { return (*static_cast<decltype(evaluate)*>(factory))(); });
}

template <class OptionsFactory>
RuntimeMain::Completion RuntimeMain::RunWithSetupAndOptions(void (*setup_runtime)(), int (*main_task)(),
                                                            int (*native_hook)(),
                                                            OptionsFactory&& make_options) noexcept
{
    const auto evaluate = [&make_options] {
        return make_options();
    };
    return RunWithFactory(setup_runtime, main_task, native_hook, nullptr, &evaluate,
                          [](const void* factory) { return (*static_cast<decltype(evaluate)*>(factory))(); });
}

template <class OptionsFactory>
RuntimeMain::Completion RuntimeMain::RunWithSetupHostHookAndOptions(void (*setup_runtime)(), int (*main_task)(),
                                                                    HostHook host_hook,
                                                                    OptionsFactory&& make_options) noexcept
{
    const auto evaluate = [&make_options] {
        return make_options();
    };
    return RunWithFactory(setup_runtime, main_task, nullptr, host_hook, &evaluate,
                          [](const void* factory) { return (*static_cast<decltype(evaluate)*>(factory))(); });
}

}
