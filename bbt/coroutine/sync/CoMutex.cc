#include <bbt/base/assert/Assert.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

namespace bbt::coroutine::sync
{

CoMutex::CoMutex()
{

}

CoMutex::~CoMutex()
{

}

void CoMutex::Lock()
{
    _SysLock();

    while (m_status == CoMutexStatus::COMUTEX_LOCKED) {
        Assert(_WaitUnLock([this](){ _SysUnLock(); return true; }) == 0);

        _SysLock();
    }

    Assert(m_status == CoMutexStatus::COMUTEX_FREE);

    m_status = CoMutexStatus::COMUTEX_LOCKED;
    _SysUnLock();
}

void CoMutex::UnLock()
{
    _SysLock();

    m_status = CoMutexStatus::COMUTEX_FREE;
    _NotifyOne();

    _SysUnLock();
}

int CoMutex::TryLock()
{

    int ret = 0;
    _SysLock();

    if (m_status == CoMutexStatus::COMUTEX_FREE)
        m_status = CoMutexStatus::COMUTEX_LOCKED;
    else
        ret = -1;

    _SysUnLock();
    return ret;
}

int CoMutex::TryLock(int ms)
{
    int ret = 0;
    _SysLock();

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

    m_status = CoMutexStatus::COMUTEX_LOCKED;
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
    auto event = g_bbt_tls_coroutine_co->RegistCustom(detail::POLL_EVENT_CUSTOM_COMUTEX);
    m_wait_event_queue.push(event);
    return g_bbt_tls_coroutine_co->YieldWithCallback([this, event, cb](){
        event->Regist();
        return cb();
    });
}

int CoMutex::_WaitUnLock(const detail::CoroutineOnYieldCallback& cb)
{
    auto event = g_bbt_tls_coroutine_co->RegistCustom(detail::POLL_EVENT_CUSTOM_COMUTEX);
    m_wait_event_queue.push(event);
    return g_bbt_tls_coroutine_co->YieldWithCallback([this, event, cb](){
        event->Regist();
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

}