#include <bbt/coroutine/runtime/RuntimeMain.hpp>
#include <atomic>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unistd.h>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Stack.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>

namespace bbt::coroutine {
namespace {

using Terminal = RuntimeMain::Terminal;
using Completion = RuntimeMain::Completion;

// 单次入口门在配置求值之前认领；setup/hook/主任务重入不能再次 Start。
std::atomic_bool s_entered{false};

class MainCompletion {
public:
    std::future<Completion> GetFuture();
    void Complete(Completion result);

private:
    std::atomic_bool m_completed{false};
    std::promise<Completion> m_promise;
};

std::future<Completion> MainCompletion::GetFuture() {
    return m_promise.get_future();
}

void MainCompletion::Complete(Completion result) {
    if (!m_completed.exchange(true, std::memory_order_acq_rel))
        m_promise.set_value(result);
}

void ApplyOptions(const RuntimeOptions& options) {
    auto& config = *detail::GlobalConfig::GetInstance();
    const auto old_workers = config.m_cfg_static_thread_num;
    const auto workers = options.initial_workers.value_or(old_workers);
    const auto stack = options.default_stack.value_or(
        StackRequest{config.m_cfg_stack_size, config.m_cfg_stack_protect});
    const auto usable = detail::Stack::NormalizeSize(stack.usable_bytes);
    const auto page = static_cast<std::size_t>(getpagesize());
    const auto max_size = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    if (stack.protect && usable > max_size - page)
        throw std::length_error("RuntimeMain: stack guard page size overflow");

    const auto policy = detail::StackPool::ValidateConfig(options.stacks.value_or(
        StackConfig{StackPolicy::Auto, {}, config.m_cfg_stackpool_max_alloc_size, 256 * 1024 * 1024}),
        {usable, stack.protect});

    config.m_cfg_static_thread_num = workers;
    detail::ProcesserScalingBounds bounds{};
    const bool valid = config.ResolveProcesserScalingBounds(bounds);
    config.m_cfg_static_thread_num = old_workers;
    if (!valid)
        throw std::invalid_argument("RuntimeMain: invalid processer scaling configuration");

    detail::StackPool::GetInstance()->Configure(policy, {usable, stack.protect});
    config.m_cfg_static_thread_num = workers;
    config.m_cfg_stack_size = usable;
    config.m_cfg_stack_protect = stack.protect;
}

} // namespace

RuntimeMain::Completion RuntimeMain::Run(int (*main_task)(), int (*native_hook)(),
                                         const RuntimeOptions& options) noexcept {
    return RunWithFactory(nullptr, main_task, native_hook, &options, [](const void* value) {
        return *static_cast<const RuntimeOptions*>(value);
    });
}

RuntimeMain::Completion RuntimeMain::RunWithSetup(void (*setup_runtime)(), int (*main_task)(),
                                                  int (*native_hook)(),
                                                  const RuntimeOptions& options) noexcept {
    return RunWithFactory(setup_runtime, main_task, native_hook, &options, [](const void* value) {
        return *static_cast<const RuntimeOptions*>(value);
    });
}

RuntimeMain::Completion RuntimeMain::RunWithFactory(void (*setup_runtime)(), int (*main_task)(),
                                                    int (*native_hook)(), const void* factory,
                                                    RuntimeOptions (*evaluate)(const void*)) noexcept {
    const Completion start_failed{Terminal::StartFailed, 1};
    if (detail::Processer::TryGetLocalProcesser() != nullptr ||
        s_entered.exchange(true, std::memory_order_acq_rel))
        return start_failed;

    auto failure = Terminal::StartFailed;
    try {
        auto completed = std::make_shared<MainCompletion>();
        auto result = completed->GetFuture();
        try {
            auto& scheduler = detail::Scheduler::GetInstance();
            if (main_task == nullptr || scheduler->IsInitialized())
                throw std::logic_error("RuntimeMain: invalid task or runtime already started");

            failure = Terminal::SetupThrew;
            if (setup_runtime != nullptr)
                setup_runtime();

            failure = Terminal::StartFailed;
            const auto options = evaluate(factory);
            ApplyOptions(options);
            detail::GlobalConfig::GetInstance()->FreezeRuntimeSetup();
            scheduler->Start();

            failure = Terminal::HookThrew;
            const int hook_code = native_hook == nullptr ? 0 : native_hook();
            if (hook_code != 0) {
                completed->Complete({Terminal::HookRejected, hook_code});
            } else {
                failure = Terminal::SubmitFailed;
                scheduler->RegistCoroutineTask([main_task, completed] {
                    Completion outcome{Terminal::MainThrew, 1};
                    try {
                        outcome = {Terminal::MainReturned, main_task()};
                    } catch (...) {
                    }
                    completed->Complete(outcome);
                }, "bbtco_main");
            }
        } catch (...) {
            completed->Complete({failure, 1});
        }
        return result.get();
    } catch (...) {
        return {failure, 1};
    }
}

} // namespace bbt::coroutine
