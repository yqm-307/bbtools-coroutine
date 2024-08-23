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
    _Lock();

    /* 如果已经上锁了，挂起并加入到等待队列中 */
    if (m_status == CoMutexStatus::COMUTEX_LOCKED) {
        Assert(_WaitUnLock([this](){ _UnLock(); return true; }) == 0);

        _Lock();
    }

    m_status = CoMutexStatus::COMUTEX_LOCKED;
    _UnLock();
}

void CoMutex::UnLock()
{
    _Lock();

    _NotifyOne();

    _UnLock();
}

int CoMutex::TryLock()
{

    int ret = 0;
    _Lock();

    if (m_status == CoMutexStatus::COMUTEX_FREE)
        m_status == CoMutexStatus::COMUTEX_LOCKED;
    else
        ret = -1;

    _UnLock();
    return ret;
}

int CoMutex::TryLock(int ms)
{
    int ret = 0;
    _Lock();

    if (m_status == CoMutexStatus::COMUTEX_LOCKED) {
        ret = _WaitUnLockUnitlTimeout(ms, [this](){ _UnLock(); return true; });

        _Lock();
    }

    m_status = CoMutexStatus::COMUTEX_LOCKED;
    _UnLock();

    return ret;
}

void CoMutex::_Lock()
{
    m_mutex.lock();
}

void CoMutex::_UnLock()
{
    m_mutex.unlock();
}

int CoMutex::_WaitUnLockUnitlTimeout(int timeout, const detail::CoroutineOnYieldCallback& cb)
{
    auto co_ptr = CoCond::Create(true);
    m_wait_lock_queue.push(std::move(co_ptr));
    return co_ptr->WaitWithTimeoutAndCallback(timeout, std::forward<const detail::CoroutineOnYieldCallback&>(cb));
}

int CoMutex::_WaitUnLock(const detail::CoroutineOnYieldCallback& cb)
{
    auto co_ptr = CoCond::Create(true);
    m_wait_lock_queue.push(std::move(co_ptr));
    return co_ptr->WaitWithCallback(std::forward<const detail::CoroutineOnYieldCallback&>(cb));
}

int CoMutex::_NotifyOne()
{
    while (true) {
        auto co_sptr = m_wait_lock_queue.front();
        m_wait_lock_queue.pop();

        if (co_sptr->Notify() == 0)
            break;
    }
}

}