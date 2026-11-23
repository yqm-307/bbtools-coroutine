#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <limits>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/pollevent/Event.hpp>

#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/WaitNode.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/DriverWakeDeferral.hpp>

namespace bbt::coroutine::sync {

using namespace bbt::coroutine::detail;

CoWaiter::SPtr CoWaiter::Create()
{
    return std::make_shared<CoWaiter>();
}

CoWaiter::CoWaiter() : m_run_status(COND_FREE) {}

CoWaiter::~CoWaiter() {}

int CoWaiter::_WaitCustom(const detail::CoroutineOnYieldCallback& cb)
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "not in coroutine!");
    auto* coroutine = g_bbt_tls_coroutine_co;
    AssertWithInfo(coroutine != nullptr, "current coroutine is nullptr!");

    std::shared_ptr<detail::WaitNode> wait_node;
    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        if (m_co_event != nullptr || m_wait_node != nullptr)
            return -1;

        wait_node = coroutine->RegistWaitNode();
        if (wait_node == nullptr)
            return -1;
        m_wait_node = wait_node;
        m_run_status = COND_WAIT;
    }

    try {
        const auto ret = coroutine->YieldWithCallback([coroutine, cb]() {
            if (!coroutine->_RegistAwaitEvent())
                return false;
            if (cb)
                cb();
            return true;
        });

        std::lock_guard<std::mutex> lock(m_notify_mutex);
        if (m_wait_node == wait_node) {
            m_wait_node = nullptr;
            m_run_status = COND_FREE;
        }
        return ret;
    } catch (...) {
        coroutine->_SetWaitNode(nullptr);
        wait_node->UnRegist();
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        if (m_wait_node == wait_node) {
            m_wait_node = nullptr;
            m_run_status = COND_FREE;
        }
        throw;
    }
}

WaitStatus CoWaiter::_WaitCustom(const WaitOptions& options, const detail::CoroutineOnYieldCallback& cb,
                                 CoEventValue* value)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return WaitStatus::InvalidContext;
    auto* coroutine = g_bbt_tls_coroutine_co;
    if (coroutine == nullptr)
        return WaitStatus::InvalidContext;
    if (coroutine->IsCancelRequested())
        return WaitStatus::Cancelled;
    if (options.deadline != Deadline::max())
        return WaitStatus::TimedOut;

    std::shared_ptr<detail::WaitNode> wait_node;
    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        if (m_co_event != nullptr) {
            if (!m_co_event->IsFinal())
                return WaitStatus::AlreadyWaiting;
            m_co_event = nullptr;
            m_run_status = COND_FREE;
        }
        if (m_wait_node != nullptr) {
            if (!m_wait_node->IsFinal())
                return WaitStatus::AlreadyWaiting;
            m_wait_node = nullptr;
            m_run_status = COND_FREE;
        }

        wait_node = coroutine->RegistWaitNode();
        if (wait_node == nullptr)
            return WaitStatus::RuntimeUnavailable;
        m_wait_node = wait_node;
        m_run_status = COND_WAIT;
    }

    int yield_ret = 0;
    try {
        yield_ret = coroutine->YieldWithCallback([coroutine, cb]() {
            if (!coroutine->_RegistAwaitEvent())
                return false;
            if (cb)
                cb();
            return true;
        });
    } catch (...) {
        coroutine->_SetWaitNode(nullptr);
        wait_node->UnRegist();
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        if (m_wait_node == wait_node) {
            m_wait_node = nullptr;
            m_run_status = COND_FREE;
        }
        throw;
    }

    const int resume_event = yield_ret == 0 ? coroutine->GetLastResumeEvent() : 0;
    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        if (m_wait_node == wait_node) {
            m_wait_node = nullptr;
            m_run_status = COND_FREE;
        }
        if (value) {
            if (yield_ret == 0 && (resume_event & POLL_EVENT_CUSTOM))
                *value = wait_node->GetNotifyValue();
            else
                *value = CoEventValue{};
        }
    }

    if (yield_ret != 0)
        return WaitStatus::RuntimeUnavailable;
    if (resume_event & POLL_EVENT_CANCELLED)
        return WaitStatus::Cancelled;
    if (resume_event & POLL_EVENT_CUSTOM)
        return WaitStatus::Completed;
    return WaitStatus::Cancelled;
}

int CoWaiter::Wait()
{
    return _WaitCustom({});
}

int CoWaiter::WaitWithCallback(const detail::CoroutineOnYieldCallback& cb)
{
    Assert(cb != nullptr);
    return _WaitCustom(cb);
}

int CoWaiter::WaitWithTimeout(int ms)
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "not in coroutine!");
    auto* coroutine = g_bbt_tls_coroutine_co;
    AssertWithInfo(coroutine != nullptr, "current coroutine is nullptr!");
    std::shared_ptr<detail::CoPollEvent> wait_event;
    int ret = 0;

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        if (m_co_event != nullptr || m_wait_node != nullptr) {
            return -1;
        }

        wait_event = coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, ms);
        m_co_event = wait_event;
        if (m_co_event == nullptr) {
            return -1;
        }

        m_run_status = COND_WAIT;
    }

    /**
     * 同 Wait() 的回滚约定。
     */
    try {
        ret = coroutine->YieldWithCallback([coroutine]() { return coroutine->_RegistAwaitEvent(); });
    } catch (...) {
        coroutine->_SetAwaitEvent(nullptr);
        {
            std::lock_guard<std::mutex> lock(m_notify_mutex);
            if (m_co_event == wait_event) {
                m_co_event = nullptr;
                m_run_status = COND_FREE;
            }
        }
        throw;
    }

    /**
     * 登记失败（ret != 0）时本协程未真正挂起，
     * 唤醒掩码仍是上一次等待的旧值，不得参与超时/取消映射。
     * 兼容映射：协程级取消现以独立取消位唤醒，旧超时族入口对取消
     * 仍上报 1，与取消借用超时位时期的可观察行为逐条一致。
     */
    if (ret == 0 && coroutine->GetLastResumeEvent() & (POLL_EVENT_TIMEOUT | POLL_EVENT_CANCELLED))
        ret = 1;

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);

        /**
         * 只解绑本次登记：窄接口的终态残留清理可能已把等待位让给后到者
         */
        if (m_co_event == wait_event) {
            m_co_event = nullptr;
            m_run_status = COND_FREE;
        }
    }
    return ret;
}

int CoWaiter::WaitWithTimeoutAndCallback(int ms, const detail::CoroutineOnYieldCallback& cb)
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "not in coroutine!");
    auto* coroutine = g_bbt_tls_coroutine_co;
    AssertWithInfo(coroutine != nullptr, "current coroutine is nullptr!");
    Assert(cb != nullptr);
    std::shared_ptr<detail::CoPollEvent> wait_event;
    int ret = 0;

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        if (m_co_event != nullptr || m_wait_node != nullptr) {
            return -1;
        }

        wait_event = coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, ms);
        m_co_event = wait_event;
        if (m_co_event == nullptr) {
            return -1;
        }

        m_run_status = COND_WAIT;
    }

    /**
     * 同 Wait() 的回滚约定。
     */
    try {
        ret = coroutine->YieldWithCallback([coroutine, cb]() {
            if (!coroutine->_RegistAwaitEvent())
                return false;
            cb();
            return true;
        });
    } catch (...) {
        coroutine->_SetAwaitEvent(nullptr);
        {
            std::lock_guard<std::mutex> lock(m_notify_mutex);
            if (m_co_event == wait_event) {
                m_co_event = nullptr;
                m_run_status = COND_FREE;
            }
        }
        throw;
    }

    /**
     * 同 WaitWithTimeout：取消位并入超时族返回值，保证旧行为不变
     */
    if (ret == 0 && coroutine->GetLastResumeEvent() & (POLL_EVENT_TIMEOUT | POLL_EVENT_CANCELLED))
        ret = 1;

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);

        /**
         * 只解绑本次登记：窄接口的终态残留清理可能已把等待位让给后到者
         */
        if (m_co_event == wait_event) {
            m_co_event = nullptr;
            m_run_status = COND_FREE;
        }
    }
    return ret;
}

WaitStatus CoWaiter::Wait(const WaitOptions& options)
{
    return WaitWithCallback(options, {}, nullptr);
}

WaitStatus CoWaiter::WaitWithCallback(const WaitOptions& options, const detail::CoroutineOnYieldCallback& on_registered,
                                      CoEventValue* value)
{
    if (options.deadline == Deadline::max())
        return _WaitCustom(options, on_registered, value);

    /**
     * 五段：入口校验 → 建事件 → 挂起 → 决议 → 解绑；任何一段失败都不得留下占位的等待位。
     * 入口校验与等待位占用判断都在 m_notify_mutex 内，与 Notify 的 COND_WAIT→COND_ACTIVE 翻转互斥。
     * 环境检查先于一切：非协程上下文一律拒绝
     */
    if (!g_bbt_tls_helper->EnableUseCo())
        return WaitStatus::InvalidContext;
    auto* coroutine = g_bbt_tls_coroutine_co;
    if (coroutine == nullptr)
        return WaitStatus::InvalidContext;

    int timeout_ms = -1;
    std::shared_ptr<detail::CoPollEvent> wait_event;
    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);

        /**
         * 入口校验顺序对齐运行时契约：已有取消先于 deadline 与等待位占用检查。
         * 预取消在此直接返回，不进入挂起；挂起中到达的取消则全部由下方
         * 唤醒掩码仲裁，两条路径产出同一 WaitStatus::Cancelled。
         */
        if (coroutine->IsCancelRequested())
            return WaitStatus::Cancelled;

        /**
         * 已过期：不注册事件（对齐运行时契约）
         */
        if (options.deadline <= std::chrono::steady_clock::now())
            return WaitStatus::TimedOut;
        if (m_co_event != nullptr) {

            /**
             * 终态残留不占等待位：前一等待的结果已在事件状态机定死，
             * 但等待者可能尚未恢复出账——若认占位，单 worker 下自旋
             * 重试者与未恢复的占位者互相饿死（恢复需要 worker，而
             * worker 正被重试方占着）。IsFinal 只读事件相位，不触碰
             * 等待者。PENDING/TRIGGERING 不算终态：决议在途仍占。
             */
            if (!m_co_event->IsFinal())
                return WaitStatus::AlreadyWaiting;
            m_co_event = nullptr;
            m_run_status = COND_FREE;
        }

        if (m_wait_node != nullptr) {
            if (!m_wait_node->IsFinal())
                return WaitStatus::AlreadyWaiting;
            m_wait_node = nullptr;
            m_run_status = COND_FREE;
        }

        /**
         * deadline 转毫秒定时：粒度上取整保证不提前超时；剩余期限达到
         * INT_MAX 毫秒（含 max() 无期限）即不挂定时器。第二次取时可能已
         * 越过 deadline，此时不能注册不会触发的 0ms 定时器。
         */
        const auto remain_ms =
            std::chrono::ceil<std::chrono::milliseconds>(options.deadline - std::chrono::steady_clock::now()).count();
        if (remain_ms <= 0)
            return WaitStatus::TimedOut;
        const bool has_timer = remain_ms < std::numeric_limits<int>::max();
        if (has_timer)
            timeout_ms = static_cast<int>(remain_ms);

        /**
         * 与 Wait 族同一等待位与登记路径：RegistCustom 只建事件不挂起，
         * 真正注册在挂起成功后的 _RegistAwaitEvent 中完成。
         */
        wait_event = has_timer ? coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, timeout_ms)
                               : coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND);
        if (wait_event == nullptr)
            return WaitStatus::RuntimeUnavailable;

        m_co_event = wait_event;

        /**
         * 本轮载荷归属本轮事件（CoPollEvent）：共享槽无需清旧值
         */
        m_run_status = COND_WAIT;
    }

    /**
     * 装配失败的同一回滚约定——协程未挂起、on_registered
     * 未执行（登记失败本就不投递），清 await 与等待位后原样上抛。
     */
    int yield_ret = 0;
    try {
        yield_ret = coroutine->YieldWithCallback([coroutine, on_registered]() {
            /**
             * 登记失败不投递回调，调用方按 RuntimeUnavailable 收尾
             */
            if (!coroutine->_RegistAwaitEvent())
                return false;
            if (on_registered)
                on_registered();
            return true;
        });
    } catch (...) {
        coroutine->_SetAwaitEvent(nullptr);
        {
            std::lock_guard<std::mutex> lock(m_notify_mutex);
            if (m_co_event == wait_event) {
                m_co_event = nullptr;
                m_run_status = COND_FREE;
            }
        }
        throw;
    }

    /**
     * 决议只读唤醒原因掩码：CoPollEvent 状态机保证仅首个胜出 Trigger 的
     * flags 被锁存并交付（PENDING 提前触发同样经掩码兑现），恢复后不按
     * 优先级重判第二遍。
     */
    const int resume_event = (yield_ret == 0) ? coroutine->GetLastResumeEvent() : 0;

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);

        /**
         * 只解绑本次登记：本事件若在恢复前已被判为终态残留清出，等待位
         * 可能已被后到等待者占用；无条件清空会抹掉后者的登记并造成丢唤醒。
         */
        if (m_co_event == wait_event) {
            m_co_event = nullptr;
            m_run_status = COND_FREE;
        }

        /**
         * 载荷与首胜原因同锁取出：只有 custom 首胜才交付载荷，定时器/取消
         * 首胜一律置空，败方 Notify 也不覆盖赢家（Notify 侧同锁只落首胜值）
         */

        /**
         * 载荷从本轮事件取出（不是共享槽）：上一轮等待的载荷留在上一轮
         * 事件对象上，不会被本轮等待覆写。
         */
        if (value) {
            if (yield_ret == 0 && (resume_event & POLL_EVENT_CUSTOM))
                *value = wait_event->GetNotifyValue();
            else
                *value = CoEventValue{};
        }
    }

    /**
     * 事件登记失败即未真正挂起，唤醒掩码仍是上一次等待的旧值，不能参与决议
     */
    if (yield_ret != 0)
        return WaitStatus::RuntimeUnavailable;

    if (resume_event & detail::POLL_EVENT_CANCELLED)
        return WaitStatus::Cancelled;
    if (resume_event & detail::POLL_EVENT_TIMEOUT)
        return WaitStatus::TimedOut;
    if (resume_event & detail::POLL_EVENT_CUSTOM)
        return WaitStatus::Completed;

    /**
     * 无明确决议位的异常唤醒按取消处理（同窄接口口径）
     */
    return WaitStatus::Cancelled;
}

CombinedWaitStatus CoWaiter::Wait(const CombinedWaitOptions& options)
{
    return Wait(options, {}, nullptr);
}

CombinedWaitStatus CoWaiter::Wait(const CombinedWaitOptions& options,
                                  const detail::CoroutineOnYieldCallback& on_registered)
{
    return Wait(options, on_registered, nullptr);
}

CombinedWaitStatus CoWaiter::Wait(const CombinedWaitOptions& options,
                                  const detail::CoroutineOnYieldCallback& on_registered, CoEventValue* value)
{
    if (!options.want_readable && !options.want_writeable && options.deadline == Deadline::max()) {
        const auto status = _WaitCustom(WaitOptions{}, on_registered, value);
        switch (status) {
        case WaitStatus::Completed:
            return CombinedWaitStatus::Completed;
        case WaitStatus::Cancelled:
            return CombinedWaitStatus::Cancelled;
        case WaitStatus::AlreadyWaiting:
            return CombinedWaitStatus::AlreadyWaiting;
        case WaitStatus::InvalidContext:
            return CombinedWaitStatus::InvalidContext;
        case WaitStatus::RuntimeUnavailable:
            return CombinedWaitStatus::RuntimeUnavailable;
        case WaitStatus::TimedOut:
            return CombinedWaitStatus::TimedOut;
        }
    }

    /**
     * 五段：选项/入口校验 → 建事件 → 挂起 → 决议 → 解绑。fd interest 与定时器必须
     * 挂在同一个 CoPollEvent 上：m_event 只能建立一次。
     *
     * 环境检查先于一切（对齐窄接口）：非协程上下文一律拒绝
     */
    if (!g_bbt_tls_helper->EnableUseCo())
        return CombinedWaitStatus::InvalidContext;
    auto* coroutine = g_bbt_tls_coroutine_co;
    if (coroutine == nullptr)
        return CombinedWaitStatus::InvalidContext;

    const bool want_fd = options.want_readable || options.want_writeable;

    /**
     * fd<0 时 fd 事件无法建立，立即拒绝；READABLE/WRITEABLE 可同时申请，
     * 底层 Event 以 OR interest 监听，恢复时按首个交付位返回原因。
     */
    if (want_fd && options.fd < 0)
        return CombinedWaitStatus::InvalidOptions;

    const short fd_listen_events = (options.want_readable ? pollevent::EventOpt::READABLE : 0)
                                   | (options.want_writeable ? pollevent::EventOpt::WRITEABLE : 0);

    int timeout_ms = -1;
    std::shared_ptr<detail::CoPollEvent> wait_event;
    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);

        /**
         * 入口校验顺序对齐窄接口：取消先于 deadline 与等待位占用检查
         */
        if (coroutine->IsCancelRequested())
            return CombinedWaitStatus::Cancelled;

        /**
         * 已过期：不注册事件
         */
        if (options.deadline <= std::chrono::steady_clock::now())
            return CombinedWaitStatus::TimedOut;
        if (m_co_event != nullptr) {

            /**
             * 终态残留不占等待位（同窄接口 Wait）：避免恢复前占位互饿
             */
            if (!m_co_event->IsFinal())
                return CombinedWaitStatus::AlreadyWaiting;
            m_co_event = nullptr;
            m_run_status = COND_FREE;
        }

        if (m_wait_node != nullptr) {
            if (!m_wait_node->IsFinal())
                return CombinedWaitStatus::AlreadyWaiting;
            m_wait_node = nullptr;
            m_run_status = COND_FREE;
        }

        const auto remain_ms =
            std::chrono::ceil<std::chrono::milliseconds>(options.deadline - std::chrono::steady_clock::now()).count();

        /**
         * 上方过期检查与 remain 计算之间可能刚好耗尽剩余期限：remain<=0
         * 时定时器口径为 0ms 不挂定时器，fd 等待会永久悬挂，按已过期拒绝
         */
        if (remain_ms <= 0)
            return CombinedWaitStatus::TimedOut;
        const bool has_timer = remain_ms < std::numeric_limits<int>::max();
        if (has_timer)
            timeout_ms = static_cast<int>(remain_ms);

        /**
         * 单事件同时承载 fd interest、custom 通知与定时器（Event 的 fd 与
         * timer 可共存）：RegistCustom 只建 custom 骨架，fd interest 与
         * 定时器经一次 InitFdEvent 挂上——同一事件 m_event 仅能建立一次，
         * 不能先 RegistCustom(timeout) 再补 fd。真正注册在挂起成功后的
         * _RegistAwaitEvent 中完成。
         */
        wait_event = coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND);
        if (wait_event == nullptr)
            return CombinedWaitStatus::RuntimeUnavailable;

        const short listen_events = fd_listen_events | (has_timer ? pollevent::EventOpt::TIMEOUT : 0);
        if (listen_events != 0 && wait_event->InitFdEvent(options.fd, listen_events, has_timer ? timeout_ms : 0) != 0) {

            /**
             * 事件建立失败：事件未注册，清掉 await 登记防协程悬挂
             */
            coroutine->_SetAwaitEvent(nullptr);
            m_co_event = nullptr;
            m_run_status = COND_FREE;
            return CombinedWaitStatus::RuntimeUnavailable;
        }

        m_co_event = wait_event;
        m_run_status = COND_WAIT;
    }

    /**
     * 同一回滚约定。
     */
    int yield_ret = 0;
    try {
        yield_ret = coroutine->YieldWithCallback([coroutine, on_registered]() {
            if (!coroutine->_RegistAwaitEvent())
                return false;
            if (on_registered)
                on_registered();
            return true;
        });
    } catch (...) {
        coroutine->_SetAwaitEvent(nullptr);
        {
            std::lock_guard<std::mutex> lock(m_notify_mutex);
            if (m_co_event == wait_event) {
                m_co_event = nullptr;
                m_run_status = COND_FREE;
            }
        }
        throw;
    }

    const int resume_event = (yield_ret == 0) ? coroutine->GetLastResumeEvent() : 0;

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        if (m_co_event == wait_event) {
            m_co_event = nullptr;
            m_run_status = COND_FREE;
        }

        /**
         * 载荷从本轮事件取出（不是共享槽）：上一轮等待的载荷留在上一轮
         * 事件对象上，不会被本轮等待覆写。
         */
        if (value) {
            if (yield_ret == 0 && (resume_event & POLL_EVENT_CUSTOM))
                *value = wait_event->GetNotifyValue();
            else
                *value = CoEventValue{};
        }
    }

    if (yield_ret != 0)
        return CombinedWaitStatus::RuntimeUnavailable;

    /**
     * 决议只读已归一化的 PollEventType 唤醒原因掩码；底层 EventOpt
     * 与运行时 PollEventType 的 READABLE/WRITEABLE 位值不同，不能混比。
     */
    if (resume_event & detail::POLL_EVENT_CANCELLED)
        return CombinedWaitStatus::Cancelled;
    if (resume_event & detail::POLL_EVENT_TIMEOUT)
        return CombinedWaitStatus::TimedOut;
    if (resume_event & detail::POLL_EVENT_CUSTOM)
        return CombinedWaitStatus::Completed;
    if (resume_event & detail::POLL_EVENT_READABLE)
        return CombinedWaitStatus::FdReadable;
    if (resume_event & detail::POLL_EVENT_WRITEABLE)
        return CombinedWaitStatus::FdWriteable;

    /**
     * 无明确决议位的异常唤醒按取消处理（同窄接口口径）
     */
    return CombinedWaitStatus::Cancelled;
}

int CoWaiter::Notify()
{

    /**
     * 没有可唤醒的登记就返回 -1，不断言：fresh（COND_DEFAULT）、FREE、
     * ACTIVE 都没有等待位。唤醒是「尝试」而不是「必须命中」，因此调用方
     * 不能把 Notify 的返回值当成完成凭据；先通知后等待的丢唤醒窗口由调用
     * 方持有的持久完成谓词关闭（登记后复查一次）。
     */

    /**
     * 在锁内锁存本轮首胜；已挂起事件的调度投递等最外层通知锁释放。
     */
    detail::DriverWakeDeferral _defer;
    std::shared_ptr<detail::WaitNode> wait_node;
    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);

        if (m_run_status != COND_WAIT)
            return -1;
        if (m_co_event != nullptr) {
            m_run_status = COND_ACTIVE;
            return m_co_event->TriggerCustom();
        }
        if (m_wait_node == nullptr)
            return -1;

        m_run_status = COND_ACTIVE;
        wait_node = m_wait_node;
    }
    return wait_node->Trigger(POLL_EVENT_CUSTOM);
}

int CoWaiter::Notify(const CoEventValue& value)
{

    /**
     * 与无参 Notify 同语义：fresh / 无等待位 / 已决议都返回 -1，不保存
     * 载荷。载荷只在本次等待确实被本事件唤醒时交付，因此不存在「把旧
     * 载荷留给下一轮等待」的问题。
     */

    /**
     * 同无参 Notify：外调推迟到锁外
     */
    detail::DriverWakeDeferral _defer;
    std::shared_ptr<detail::WaitNode> wait_node;
    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);

        if (m_run_status != COND_WAIT)
            return -1;
        if (m_co_event != nullptr) {
            m_co_event->SetNotifyValue(value);
            m_run_status = COND_ACTIVE;
            return m_co_event->TriggerCustom();
        }
        if (m_wait_node == nullptr)
            return -1;

        m_wait_node->SetNotifyValue(value);
        m_run_status = COND_ACTIVE;
        wait_node = m_wait_node;
    }
    return wait_node->Trigger(POLL_EVENT_CUSTOM);
}

}
