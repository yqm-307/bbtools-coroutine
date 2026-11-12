#include <bbt/coroutine/runtime/RuntimeMain.hpp>
#include <atomic>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
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

} // namespace

namespace detail {

// 提交后 host hook 与主任务完成共享的单槽状态。登记决定（替换 / 迟到）与完成发布
// （置完成位并取槽）整体落在同一临界区内，锁的占用就是唯一线性化点：完成位可见
// 必然等价于槽位已被取走，完成路径不会再执行一次已被替换的旧登记。
class HostHookState {
public:
    void Register(std::function<void()> callback) noexcept {
        std::function<void()> taken;
        bool late = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_main_done) {
                // 完成发布先线性化：本登记一律按迟到处理，取到锁外在本线程回调。
                taken = std::move(callback);
                late = true;
            } else {
                // 单槽替换：前一次未被取走的登记就此作废，只在锁外析构。
                taken.swap(m_callback);
                m_callback = std::move(callback);
            }
        }
        if (late)
            Invoke(taken);
    }

    bool MainDone() const noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_main_done;
    }

    // 主任务完成：完成位与取槽在同一临界区内提交，其后到来的登记全是迟到登记。
    void PublishMainDone() noexcept {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_main_done)
                return; // 完成只发起一次
            m_main_done = true;
            callback.swap(m_callback);
        }
        Invoke(callback);
    }

    // hook 退出：丢弃未取出的登记（锁外析构）；已取出的在途回调不在这里等待或撤销。
    void Detach() noexcept {
        std::function<void()> dropped;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            dropped.swap(m_callback);
        }
    }

private:
    // 回调是 hook 代码：异常不得穿过 runtime 边界，也不得因此丢完成记录。
    static void Invoke(const std::function<void()>& callback) noexcept {
        if (!callback)
            return;
        try {
            callback();
        } catch (...) {
        }
    }

    mutable std::mutex m_mutex;
    bool m_main_done{false};
    std::function<void()> m_callback;
};

} // namespace detail

HostHookContext::HostHookContext(std::shared_ptr<detail::HostHookState> state)
    : m_state(std::move(state)) {}

void HostHookContext::OnMainTaskDone(std::function<void()> callback) noexcept {
    m_state->Register(std::move(callback));
}

bool HostHookContext::MainTaskDone() const noexcept {
    return m_state->MainDone();
}

namespace {

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
    return RunWithFactory(nullptr, main_task, native_hook, nullptr, &options, [](const void* value) {
        return *static_cast<const RuntimeOptions*>(value);
    });
}

RuntimeMain::Completion RuntimeMain::RunWithSetup(void (*setup_runtime)(), int (*main_task)(),
                                                  int (*native_hook)(),
                                                  const RuntimeOptions& options) noexcept {
    return RunWithFactory(setup_runtime, main_task, native_hook, nullptr, &options,
                          [](const void* value) {
                              return *static_cast<const RuntimeOptions*>(value);
                          });
}

RuntimeMain::Completion RuntimeMain::RunWithSetupAndHostHook(void (*setup_runtime)(),
                                                             int (*main_task)(),
                                                             HostHook host_hook,
                                                             const RuntimeOptions& options) noexcept {
    return RunWithFactory(setup_runtime, main_task, nullptr, host_hook, &options,
                          [](const void* value) {
                              return *static_cast<const RuntimeOptions*>(value);
                          });
}

RuntimeMain::Completion RuntimeMain::RunWithFactory(void (*setup_runtime)(),
                                                    int (*main_task)(), int (*native_hook)(),
                                                    HostHook host_hook, const void* factory,
                                                    RuntimeOptions (*evaluate)(const void*)) noexcept {
    const Completion start_failed{Terminal::StartFailed, 1};
    if (detail::Processer::TryGetLocalProcesser() != nullptr ||
        s_entered.exchange(true, std::memory_order_acq_rel))
        return start_failed;

    auto failure = Terminal::StartFailed;
    try {
        auto completed = std::make_shared<MainCompletion>();
        std::shared_ptr<detail::HostHookState> host_state;
        if (host_hook != nullptr)
            host_state = std::make_shared<detail::HostHookState>();
        auto result = completed->GetFuture();
        bool submitted = false;
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
                scheduler->RegistCoroutineTask([main_task, completed, host_state] {
                    Completion outcome{Terminal::MainThrew, 1};
                    try {
                        outcome = {Terminal::MainReturned, main_task()};
                    } catch (...) {
                    }
                    // 通知先于完成记录：runtime 返回时不存在本登记的在途回调。
                    if (host_state)
                        host_state->PublishMainDone();
                    completed->Complete(outcome);
                }, "bbtco_main");
                submitted = true;
            }
        } catch (...) {
            completed->Complete({failure, 1});
        }
        if (!submitted)
            return result.get();

        Terminal host_terminal = Terminal::MainReturned;
        int host_code = 0;
        if (host_hook != nullptr) {
            HostHookContext context{host_state};
            try {
                host_code = host_hook(context);
                if (host_code != 0)
                    host_terminal = Terminal::HostHookFailed;
            } catch (...) {
                host_terminal = Terminal::HostHookThrew;
                host_code = 1;
            }
            // 已取出的回调可能在 hook 返回后仍在执行；未取出的登记就此丢弃。
            host_state->Detach();
        }

        const auto main_outcome = result.get();
        if (host_terminal == Terminal::HostHookFailed)
            return {Terminal::HostHookFailed, host_code};
        if (host_terminal == Terminal::HostHookThrew)
            return {Terminal::HostHookThrew, 1};
        if (main_outcome.terminal == Terminal::MainThrew)
            return {Terminal::MainThrew, 1};
        return main_outcome;
    } catch (...) {
        return {failure, 1};
    }
}

} // namespace bbt::coroutine
