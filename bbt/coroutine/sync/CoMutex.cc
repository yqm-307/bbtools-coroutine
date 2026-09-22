#include <bbt/core/util/Assert.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>

namespace bbt::coroutine::sync
{

CoMutex::SPtr CoMutex::Create()
{
    return std::make_shared<CoMutex>(PrivateTag{});
}

CoMutex::CoMutex(PrivateTag)
{

}

CoMutex::~CoMutex()
{

}

void CoMutex::Lock()
{
    _SysLock();
    
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "must use in coroutine!");
    AssertWithInfo(m_locked_co != g_bbt_tls_coroutine_co, "comutex deadlock!");

    while (m_status == CoMutexStatus::COMUTEX_LOCKED) {
#if defined(BBT_COROUTINE_PROFILE)
        detail::Profiler::GetInstance()->OnEvent_CoMutexLockYield();
#endif
        Assert(_WaitUnLock([this](){ _SysUnLock(); return true; }) == 0);

        _SysLock();
    }

    _OnLocked();
    _SysUnLock();
}

void CoMutex::UnLock()
{
    _SysLock();

    _OnUnLocked();

    _SysUnLock();
}

int CoMutex::TryLock()
{

    int ret = 0;
    _SysLock();
    AssertWithInfo(m_locked_co != g_bbt_tls_coroutine_co, "comutex deadlock!");

    if (m_status == CoMutexStatus::COMUTEX_FREE)
        _OnLocked();
    else
        ret = -1;

    _SysUnLock();
    return ret;
}

int CoMutex::TryLock(int ms)
{
    int ret = 0;
    _SysLock();
    AssertWithInfo(m_locked_co != g_bbt_tls_coroutine_co, "comutex deadlock!");

    if (m_status == CoMutexStatus::COMUTEX_LOCKED) {
        ret = _WaitUnLockUnitlTimeout(ms, [this](){ _SysUnLock(); return true; });

        _SysLock();
    }

    if (ret != 0) {
        _SysUnLock();
        return ret;
    }

    if (m_status != CoMutexStatus::COMUTEX_FREE) {
        _SysUnLock();
        return -1;
    }

    _OnLocked();
    _SysUnLock();
    return 0;
}

void CoMutex::_SysLock()
{
    m_mutex.lock();
}

void CoMutex::_SysUnLock()
{
    m_mutex.unlock();
}

int CoMutex::_WaitUnLockUnitlTimeout(int timeout, const detail::CoroutineOnYieldCallback& cb)
{
    auto* coroutine = g_bbt_tls_coroutine_co;
    AssertWithInfo(coroutine != nullptr, "current coroutine is nullptr!");
    auto event = coroutine->RegistCustom(detail::POLL_EVENT_CUSTOM_COMUTEX, timeout);
    if (event == nullptr)
        return -1;
    m_wait_event_queue.push(event);
    int ret = coroutine->YieldWithCallback([coroutine, cb](){
        if (!coroutine->_RegistAwaitEvent())
            return false;
        return cb();
    });

    /* #347② 兼容映射：协程级取消现以独立取消位唤醒（原借用超时位），
     * 本处把取消仍按超时上报 1，保持 TryLock(ms) 既有可观察行为不变。 */
    if (coroutine->GetLastResumeEvent() & (detail::POLL_EVENT_TIMEOUT | detail::POLL_EVENT_CANCELLED))
        return 1;  // timeout

    return ret;
}

int CoMutex::_WaitUnLock(const detail::CoroutineOnYieldCallback& cb)
{
    auto* coroutine = g_bbt_tls_coroutine_co;
    AssertWithInfo(coroutine != nullptr, "current coroutine is nullptr!");
    auto event = coroutine->RegistCustom(detail::POLL_EVENT_CUSTOM_COMUTEX);
    if (event == nullptr)
        return -1;
    m_wait_event_queue.push(event);
    return coroutine->YieldWithCallback([coroutine, cb](){
        if (!coroutine->_RegistAwaitEvent())
            return false;
        return cb();
    });
}

void CoMutex::_NotifyOne()
{
    while (!m_wait_event_queue.empty()) {
        auto co_sptr = m_wait_event_queue.front();
        m_wait_event_queue.pop();
        Assert(co_sptr != nullptr);
        if (co_sptr->Trigger(detail::POLL_EVENT_CUSTOM) == 0)
            break;
    }
}

void CoMutex::_OnLocked()
{
    Assert(m_status == CoMutexStatus::COMUTEX_FREE);
    m_locked_co = g_bbt_tls_coroutine_co;
    m_status = CoMutexStatus::COMUTEX_LOCKED;
}

void CoMutex::_OnUnLocked()
{
    m_status = CoMutexStatus::COMUTEX_FREE;
    _NotifyOne();
    m_locked_co = nullptr;
}

}