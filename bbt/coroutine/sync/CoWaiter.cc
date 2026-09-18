#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <limits>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>

namespace bbt::coroutine::sync
{

using namespace bbt::coroutine::detail;

CoWaiter::SPtr CoWaiter::Create()
{
    return std::make_shared<CoWaiter>();
}


CoWaiter::CoWaiter():
    m_run_status(COND_FREE)
{
}

CoWaiter::~CoWaiter()
{
}

int CoWaiter::Wait()
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "not in coroutine!");
    auto* coroutine = g_bbt_tls_coroutine_co;
    AssertWithInfo(coroutine != nullptr, "current coroutine is nullptr!");
    std::shared_ptr<detail::CoPollEvent> wait_event;

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        /* 保证只有一个协程可以成功挂起 */
        if (m_co_event != nullptr) {
            return -1;
        }

        wait_event = coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND);
        m_co_event = wait_event;
        if (m_co_event == nullptr) {
            return -1;
        }

        m_run_status = COND_WAIT;
    }

    /* #339：必须经 Coroutine::_RegistAwaitEvent 登记——该公共路径在事件挂上前
     * 检查 IsCancelRequested（预取消立即自唤醒），成功后把协程纳入 parked 登记，
     * Scheduler::Stop 才有回收点。直接 event->Regist() 会绕过两者：
     * 预取消协程无限挂起、Stop 后任务闭包与栈泄漏。 */
    coroutine->YieldWithCallback([coroutine](){
        return coroutine->_RegistAwaitEvent();
    });

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        /* 只解绑本次登记：窄接口的终态残留清理可能已把等待位让给后到者 */
        if (m_co_event == wait_event) {
            m_co_event = nullptr;
            m_run_status = COND_FREE;
        }
    }
    return 0;
}

int CoWaiter::WaitWithCallback(const detail::CoroutineOnYieldCallback& cb)
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "not in coroutine!");
    auto* coroutine = g_bbt_tls_coroutine_co;
    AssertWithInfo(coroutine != nullptr, "current coroutine is nullptr!");
    Assert(cb != nullptr);
    std::shared_ptr<detail::CoPollEvent> wait_event;

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        if (m_co_event != nullptr) {
            return -1;
        }

        wait_event = coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND);
        m_co_event = wait_event;
        if (m_co_event == nullptr) {
            return -1;
        }

        m_run_status = COND_WAIT;
    }

    /* #339：同 Wait()，走公共登记路径；cb 保持"挂起后、唤醒前"的既有语义 */
    int ret = coroutine->YieldWithCallback([coroutine, cb](){
        if (!coroutine->_RegistAwaitEvent())
            return false;
        cb();
        return true;
    });

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        /* 只解绑本次登记：窄接口的终态残留清理可能已把等待位让给后到者 */
        if (m_co_event == wait_event) {
            m_co_event = nullptr;
            m_run_status = COND_FREE;
        }
    }
    return ret;
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
        if (m_co_event != nullptr) {
            return -1;
        }

        wait_event = coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, ms);
        m_co_event = wait_event;
        if (m_co_event == nullptr) {
            return -1;
        }

        m_run_status = COND_WAIT;
    }

    ret = coroutine->YieldWithCallback([coroutine](){
        return coroutine->_RegistAwaitEvent();
    });

    /* #347② 兼容映射：协程级取消现以独立取消位唤醒，旧超时族入口对取消
     * 仍上报 1，与取消借用超时位时期的可观察行为逐条一致。 */
    if (coroutine->GetLastResumeEvent() & (POLL_EVENT_TIMEOUT | POLL_EVENT_CANCELLED))
        ret = 1;

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        /* 只解绑本次登记：窄接口的终态残留清理可能已把等待位让给后到者 */
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
        if (m_co_event != nullptr) {
            return -1;
        }

        wait_event = coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, ms);
        m_co_event = wait_event;
        if (m_co_event == nullptr) {
            return -1;
        }

        m_run_status = COND_WAIT;
    }

    ret = coroutine->YieldWithCallback([coroutine, cb](){
        if (!coroutine->_RegistAwaitEvent())
            return false;
        cb();
        return true;
    });

    /* 同 WaitWithTimeout：取消位并入超时族返回值，保证旧行为不变 */
    if (ret == 0 && coroutine->GetLastResumeEvent() & (POLL_EVENT_TIMEOUT | POLL_EVENT_CANCELLED))
        ret = 1;

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        /* 只解绑本次登记：窄接口的终态残留清理可能已把等待位让给后到者 */
        if (m_co_event == wait_event) {
            m_co_event = nullptr;
            m_run_status = COND_FREE;
        }
    }
    return ret;
}

WaitStatus CoWaiter::Wait(const WaitOptions& options)
{
    /* 环境检查先于一切（对齐 §C1）：非协程上下文一律拒绝 */
    if (!g_bbt_tls_helper->EnableUseCo())
        return WaitStatus::InvalidContext;
    auto* coroutine = g_bbt_tls_coroutine_co;
    if (coroutine == nullptr)
        return WaitStatus::InvalidContext;

    int timeout_ms = -1;
    std::shared_ptr<detail::CoPollEvent> wait_event;
    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        /* 入口校验顺序对齐 §C1：已有取消先于 deadline 与等待位占用检查。
         * 预取消在此直接返回，不进入挂起；挂起中到达的取消则全部由下方
         * 唤醒掩码仲裁，两条路径产出同一 WaitStatus::Cancelled。 */
        if (options.cancel.IsCancellationRequested() || coroutine->IsCancelRequested())
            return WaitStatus::Cancelled;
        if (options.deadline <= std::chrono::steady_clock::now())
            return WaitStatus::TimedOut;    /* 已过期：不注册事件（对齐 §C1） */
        if (m_co_event != nullptr) {
            /* 终态残留不占等待位：前一等待的结果已在事件状态机定死，
             * 但等待者可能尚未恢复出账——若认占位，单 worker 下自旋
             * 重试者与未恢复的占位者互相饿死（恢复需要 worker，而
             * worker 正被重试方占着）。IsFinal 只读事件相位，不触碰
             * 等待者。PENDING/TRIGGERING 不算终态：决议在途仍占。 */
            if (!m_co_event->IsFinal())
                return WaitStatus::AlreadyWaiting;
            m_co_event = nullptr;
            m_run_status = COND_FREE;
        }

        /* deadline 转毫秒定时：粒度上取整保证不提前超时；剩余期限达到
         * INT_MAX 毫秒（含 max() 无期限）即不挂定时器（同 CompletionSignal）。 */
        const auto remain_ms = std::chrono::ceil<std::chrono::milliseconds>(
            options.deadline - std::chrono::steady_clock::now()).count();
        const bool has_timer = remain_ms < std::numeric_limits<int>::max();
        if (has_timer)
            timeout_ms = static_cast<int>(remain_ms);

        /* 与 Wait 族同一等待位与登记路径：RegistCustom 只建事件不挂起，
         * 真正注册在挂起成功后的 _RegistAwaitEvent 中完成。 */
        wait_event = has_timer
            ? coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, timeout_ms)
            : coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND);
        if (wait_event == nullptr)
            return WaitStatus::RuntimeUnavailable;

        m_co_event = wait_event;
        m_run_status = COND_WAIT;
    }

    /* 向取消令牌登记唤醒目标：回调持事件 shared_ptr，事件终态后 Trigger
     * 为 no-op，晚到订阅不触碰已结束的等待（t_late_subscription）。令牌已
     * 取消时 Register 在调用线程同步回调——此刻事件尚未注册，触发落在
     * INITED 阶段由 PENDING 吸收，CommitPark 消费后仍按取消掩码决议。 */
    const auto cb_id = options.cancel._RegisterCancelCallback([wait_event]() {
        g_bbt_poller->NotifyCancelEvent(wait_event);
    });

    const int yield_ret = coroutine->YieldWithCallback([coroutine]() {
        return coroutine->_RegistAwaitEvent();
    });

    options.cancel._UnregisterCancelCallback(cb_id);

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        /* 只解绑本次登记：本事件若在恢复前已被判为终态残留清出，等待位
         * 可能已被后到等待者占用，无条件清空会抹掉后者的登记使其丢唤醒
         * （同 CompletionSignal Critical-1 修复模式）。 */
        if (m_co_event == wait_event) {
            m_co_event = nullptr;
            m_run_status = COND_FREE;
        }
    }

    /* 事件登记失败即未真正挂起，唤醒掩码仍是上一次等待的旧值，不能参与决议 */
    if (yield_ret != 0)
        return WaitStatus::RuntimeUnavailable;

    /* 决议只读唤醒原因掩码：CoPollEvent 状态机保证仅首个胜出 Trigger 的
     * flags 被锁存并交付（PENDING 提前触发同样经掩码兑现），恢复后不按
     * 优先级重判第二遍。 */
    const int resume_event = coroutine->GetLastResumeEvent();
    if (resume_event & detail::POLL_EVENT_CANCELLED)
        return WaitStatus::Cancelled;
    if (resume_event & detail::POLL_EVENT_TIMEOUT)
        return WaitStatus::TimedOut;
    if (resume_event & detail::POLL_EVENT_CUSTOM)
        return WaitStatus::Completed;
    /* 无明确决议位的异常唤醒按取消处理（沿用 CompletionSignal 兜底先例） */
    return WaitStatus::Cancelled;
}

int CoWaiter::Notify()
{
    Assert(m_run_status != COND_DEFAULT);
    std::lock_guard<std::mutex> lock(m_notify_mutex);
    
    if (m_co_event == nullptr || m_run_status != COND_WAIT) {
        return -1;
    }

    m_run_status = COND_ACTIVE;
    return g_bbt_poller->NotifyCustomEvent(m_co_event);
}

}