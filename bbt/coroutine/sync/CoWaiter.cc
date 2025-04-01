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


    /* 保证只有一个协程可以成功挂起 */
    if (m_co_event != nullptr) {
        return -1;
    }

    m_co_event = g_bbt_tls_coroutine_co->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND);
    if (m_co_event == nullptr) {
        return -1;
    }

    m_run_status = COND_WAIT;

    g_bbt_tls_coroutine_co->YieldWithCallback([this](){ 
        Assert(m_co_event->Regist() == 0);
        return true; 
    });

    m_co_event = nullptr;
    m_run_status = COND_FREE;
    return 0;
}

int CoWaiter::WaitWithCallback(const detail::CoroutineOnYieldCallback& cb)
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "not in coroutine!");
    Assert(cb != nullptr);

    if (m_co_event != nullptr) {
        return -1;
    }
        
    m_co_event = g_bbt_tls_coroutine_co->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND);
    if (m_co_event == nullptr) {
        return -1;
    }
        
    m_run_status = COND_WAIT;

    int ret = g_bbt_tls_coroutine_co->YieldWithCallback([this, cb](){
        Assert(m_co_event->Regist() == 0);
        cb();
        return true; 
    });

    m_co_event = nullptr;
    m_run_status = COND_FREE;
    return ret;
}

int CoWaiter::WaitWithTimeout(int ms)
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "not in coroutine!");
    int ret = 0;


    if (m_co_event != nullptr) {
        return -1;
    }

    m_co_event = g_bbt_tls_coroutine_co->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, ms);
    if (m_co_event == nullptr) {
        return -1;
    }
    
    m_run_status = COND_WAIT;

    ret = g_bbt_tls_coroutine_co->YieldWithCallback([this](){
        Assert(m_co_event->Regist() == 0);
        return true; 
    });

    if (g_bbt_tls_coroutine_co->GetLastResumeEvent() & POLL_EVENT_TIMEOUT)
        ret = 1;

    m_co_event = nullptr;
    m_run_status = COND_FREE;
    return ret;
}

int CoWaiter::WaitWithTimeoutAndCallback(int ms, const detail::CoroutineOnYieldCallback& cb)
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "not in coroutine!");
    int ret = 0;

    if (m_co_event != nullptr) {
        return -1;
    }

    m_co_event = g_bbt_tls_coroutine_co->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, ms);
    if (m_co_event == nullptr) {
        return -1;
    }

    m_run_status = COND_WAIT;

    ret = g_bbt_tls_coroutine_co->YieldWithCallback([this, cb](){
        Assert(m_co_event->Regist() == 0);
        cb();
        return true;
    });

    if (ret == 0 && g_bbt_tls_coroutine_co->GetLastResumeEvent() & POLL_EVENT_TIMEOUT)
        ret = 1;
    
    m_co_event = nullptr;
    m_run_status = COND_FREE;
    return ret;
}

int CoWaiter::Notify()
{
    Assert(m_run_status != COND_DEFAULT);;
    if (m_co_event == nullptr || m_run_status != COND_WAIT) {
        return -1;
    }

    m_run_status = COND_ACTIVE;
    return g_bbt_poller->NotifyCustomEvent(m_co_event);
}

}