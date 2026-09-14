#include <unistd.h>
#include <fcntl.h>
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

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        /* 保证只有一个协程可以成功挂起 */
        if (m_co_event != nullptr) {
            return -1;
        }

        m_co_event = coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND);
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
        m_co_event = nullptr;
        m_run_status = COND_FREE;
    }
    return 0;
}

int CoWaiter::WaitWithCallback(const detail::CoroutineOnYieldCallback& cb)
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "not in coroutine!");
    auto* coroutine = g_bbt_tls_coroutine_co;
    AssertWithInfo(coroutine != nullptr, "current coroutine is nullptr!");
    Assert(cb != nullptr);

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        if (m_co_event != nullptr) {
            return -1;
        }
            
        m_co_event = coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND);
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
        m_co_event = nullptr;
        m_run_status = COND_FREE;
    }
    return ret;
}

int CoWaiter::WaitWithTimeout(int ms)
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "not in coroutine!");
    auto* coroutine = g_bbt_tls_coroutine_co;
    AssertWithInfo(coroutine != nullptr, "current coroutine is nullptr!");
    int ret = 0;

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        if (m_co_event != nullptr) {
            return -1;
        }

        m_co_event = coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, ms);
        if (m_co_event == nullptr) {
            return -1;
        }
        
        m_run_status = COND_WAIT;
    }

    ret = coroutine->YieldWithCallback([coroutine](){
        return coroutine->_RegistAwaitEvent();
    });

    if (coroutine->GetLastResumeEvent() & POLL_EVENT_TIMEOUT)
        ret = 1;

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        m_co_event = nullptr;
        m_run_status = COND_FREE;
    }
    return ret;
}

int CoWaiter::WaitWithTimeoutAndCallback(int ms, const detail::CoroutineOnYieldCallback& cb)
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "not in coroutine!");
    auto* coroutine = g_bbt_tls_coroutine_co;
    AssertWithInfo(coroutine != nullptr, "current coroutine is nullptr!");
    Assert(cb != nullptr);
    int ret = 0;

    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        if (m_co_event != nullptr) {
            return -1;
        }

        m_co_event = coroutine->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, ms);
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

    if (ret == 0 && coroutine->GetLastResumeEvent() & POLL_EVENT_TIMEOUT)
        ret = 1;
    
    {
        std::lock_guard<std::mutex> lock(m_notify_mutex);
        m_co_event = nullptr;
        m_run_status = COND_FREE;
    }
    return ret;
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