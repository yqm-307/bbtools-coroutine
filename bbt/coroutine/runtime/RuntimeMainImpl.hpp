/** @file RuntimeMainImpl.hpp
 * @brief 配置值工厂的同步转发；只由 RuntimeMain.hpp 包含。
 */
#pragma once

namespace bbt::coroutine {

template <class OptionsFactory>
RuntimeMain::Completion RuntimeMain::RunWithOptions(int (*main_task)(), int (*native_hook)(),
                                                    OptionsFactory&& make_options) noexcept {
    // 借用 callable（也支持函数左值/不可复制工厂），不分配或复制配置。
    const auto evaluate = [&make_options] { return make_options(); };
    return RunWithFactory(nullptr, main_task, native_hook, &evaluate, [](const void* factory) {
        return (*static_cast<decltype(evaluate)*>(factory))();
    });
}

template <class OptionsFactory>
RuntimeMain::Completion RuntimeMain::RunWithSetupAndOptions(void (*setup_runtime)(),
                                                            int (*main_task)(),
                                                            int (*native_hook)(),
                                                            OptionsFactory&& make_options) noexcept {
    const auto evaluate = [&make_options] { return make_options(); };
    return RunWithFactory(setup_runtime, main_task, native_hook, &evaluate, [](const void* factory) {
        return (*static_cast<decltype(evaluate)*>(factory))();
    });
}

} // namespace bbt::coroutine
