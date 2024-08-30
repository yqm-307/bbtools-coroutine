#include <unistd.h>
#include <fcntl.h>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>

namespace bbt::coroutine::sync
{

using namespace bbt::coroutine::detail;

CoCond::SPtr CoCond::Create(bool nolock)
{
    return std::make_shared<CoCond>(nolock);
}


CoCond::CoCond(bool nolock):
    m_co_event_mutex(nolock ? nullptr : new std::mutex()),
    m_run_status(COND_FREE)
{
}

CoCond::~CoCond()
{
    if (m_co_event_mutex != nullptr)
        delete m_co_event_mutex;
}

int CoCond::Wait()
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "please use CoCond in coroutine!"); // 请在协程中使用CoCond
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "running a non-corourine!");      // 当前运行的非协程

    _Lock();

    /* 保证只有一个协程可以成功挂起 */
    if (m_co_event != nullptr) {
        _UnLock();
        return -1;
    }

    m_co_event = g_bbt_tls_coroutine_co->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND);
    if (m_co_event == nullptr) {
        _UnLock();
        return -1;
    }

    m_run_status = COND_WAIT;

    g_bbt_tls_coroutine_co->YieldWithCallback([this](){ 
        Assert(m_co_event->Regist() == 0);
        _UnLock();
        return true; 
    });

    _Lock();
    m_co_event = nullptr;
    m_run_status = COND_FREE;
    _UnLock();
    return 0;
}

int CoCond::WaitWithCallback(const detail::CoroutineOnYieldCallback& cb)
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "please use CoCond in coroutine!"); // 请在协程中使用CoCond
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "running a non-corourine!");      // 当前运行的非协程
    Assert(cb != nullptr);

    _Lock();
    if (m_co_event != nullptr) {
        _UnLock();
        return -1;
    }
        
    m_co_event = g_bbt_tls_coroutine_co->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND);
    if (m_co_event == nullptr) {
        _UnLock();
        return -1;
    }
        
    m_run_status = COND_WAIT;

    int ret = g_bbt_tls_coroutine_co->YieldWithCallback([this, cb](){
        Assert(m_co_event->Regist() == 0);
        _UnLock();
        cb();
        return true; 
    });

    _Lock();
    m_co_event = nullptr;
    m_run_status = COND_FREE;
    _UnLock();
    return ret;
}

int CoCond::WaitWithTimeout(int ms)
{
    int ret = 0;

    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "please use CoCond in coroutine!"); // 请在协程中使用CoCond
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "running a non-corourine!");      // 当前运行的非协程

    _Lock();
    if (m_co_event != nullptr) {
        _UnLock();
        return -1;
    }

    m_co_event = g_bbt_tls_coroutine_co->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, ms);
    if (m_co_event == nullptr) {
        _UnLock();
        return -1;
    }
    
    m_run_status = COND_WAIT;

    ret = g_bbt_tls_coroutine_co->YieldWithCallback([this](){
        Assert(m_co_event->Regist() == 0);
        _UnLock();
        return true; 
    });

    if (g_bbt_tls_coroutine_co->GetLastResumeEvent() & POLL_EVENT_TIMEOUT)
        ret = 1;

    _Lock();
    m_co_event = nullptr;
    m_run_status = COND_FREE;
    _UnLock();
    return ret;
}

int CoCond::WaitWithTimeoutAndCallback(int ms, const detail::CoroutineOnYieldCallback& cb)
{
    int ret = 0;

    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "please use CoCond in coroutine!"); // 请在协程中使用CoCond
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "running a non-corourine!");      // 当前运行的非协程

    _Lock();
    if (m_co_event != nullptr) {
        _UnLock();
        return -1;
    }

    m_co_event = g_bbt_tls_coroutine_co->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, ms);
    if (m_co_event == nullptr) {
        _UnLock();
        return -1;
    }

    m_run_status = COND_WAIT;

    ret = g_bbt_tls_coroutine_co->YieldWithCallback([this, cb](){
        Assert(m_co_event->Regist() == 0);
        _UnLock();
        cb();
        return true;
    });

    if (ret == 0 && g_bbt_tls_coroutine_co->GetLastResumeEvent() & POLL_EVENT_TIMEOUT)
        ret = 1;
    
    _Lock();
    m_co_event = nullptr;
    m_run_status = COND_FREE;
    _UnLock();
    return ret;
}

int CoCond::Notify()
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "please use CoCond in coroutine!");

    _Lock();
    Assert(m_run_status != COND_DEFAULT);;
    if (m_co_event == nullptr || m_run_status != COND_WAIT) {
        _UnLock();
        return -1;
    }

    m_run_status = COND_ACTIVE;
    g_bbt_poller->NotifyCustomEvent(m_co_event);

    _UnLock();
    return 0;
}

void CoCond::_Lock()
{
    if (m_co_event_mutex != nullptr)
        m_co_event_mutex->lock();
}

void CoCond::_UnLock()
{
    if (m_co_event_mutex != nullptr)
        m_co_event_mutex->unlock();
}


}