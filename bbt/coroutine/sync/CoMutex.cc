#include <bbt/coroutine/sync/CoMutex.hpp>
#include <bbt/base/assert/Assert.hpp>

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

    /* 如果已经上锁了，挂起并加入到等待队列中 */
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
    auto co_ptr = CoCond::Create();
    m_wait_lock_queue.push(co_ptr);
    return co_ptr->WaitWithTimeoutAndCallback(timeout, std::forward<const detail::CoroutineOnYieldCallback&>(cb));
}

int CoMutex::_WaitUnLock(const detail::CoroutineOnYieldCallback& cb)
{
    auto co_ptr = CoCond::Create();
    m_wait_lock_queue.push(co_ptr);
    return co_ptr->WaitWithCallback(std::forward<const detail::CoroutineOnYieldCallback&>(cb));
}

void CoMutex::_NotifyOne()
{
    while (!m_wait_lock_queue.empty()) {
        auto co_sptr = m_wait_lock_queue.front();
        m_wait_lock_queue.pop();
        Assert(co_sptr != nullptr);
        if (co_sptr->Notify() == 0)
            break;
    }
}

}