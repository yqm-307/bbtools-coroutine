#include <bbt/coroutine/sync/CoRWMutex.hpp>

namespace bbt::coroutine::sync
{

CoRWMutex::SPtr CoRWMutex::Create()
{
    return std::make_shared<CoRWMutex>(PrivateTag{});
}


int CoRWMutex::RLock()
{
    _SysLock();
    // 如果获取不到锁则持续挂起，直到获取到锁
    while (m_status == CORWMUTEX_WLOCKED || m_has_wait_wlock) {
        _WaitRLock([this](){
            _SysUnLock();
            return true;
        });

        _SysLock();
    }

    /* 读锁获取成功 */
    m_status = CORWMUTEX_RLOCKED;
    m_rlock_hold_num++;

    /* 如果有等待中的写锁，那么就不在唤醒读锁了 */
    if (!m_has_wait_wlock)
        _NotifyAll(true);

    _SysUnLock();
    return 0;
}

int CoRWMutex::WLock()
{
    _SysLock();
    while (m_status != CORWMUTEX_FREE) {
        m_has_wait_wlock = true;
        _WaitWLock([this](){
            _SysUnLock();
            return true;
        });

        _SysLock();
    }

    m_has_wait_wlock = false;
    m_status = CORWMUTEX_WLOCKED;
    _SysUnLock();
    return 0;
}

int CoRWMutex::RUnLock()
{
    _SysLock();

    m_rlock_hold_num--;
    m_status = (m_rlock_hold_num == 0) ? CORWMUTEX_FREE : CORWMUTEX_RLOCKED;

    if (m_status == CORWMUTEX_FREE)
        _NotifyOne();

    _SysUnLock();
    return 0;
}

int CoRWMutex::WUnLock()
{
    _SysLock();

    m_status = CORWMUTEX_FREE;

    if (!m_wait_writelock_queue.empty())
        _NotifyOne();
    else if (!m_wait_readlock_queue.empty())
        _NotifyAll(true);

    _SysUnLock();
    return 0;
}

int CoRWMutex::TryRLock()
{
    _SysLock();
    if (m_status == CORWMUTEX_WLOCKED || m_has_wait_wlock) {
        _SysUnLock();
        return -1;
    }

    m_status = CORWMUTEX_RLOCKED;
    m_rlock_hold_num++;

    if (!m_has_wait_wlock)
        _NotifyAll(true);

    _SysUnLock();
    return 0;
}

int CoRWMutex::TryRLock(int ms)
{
    _SysLock();
    if (m_status != CORWMUTEX_WLOCKED && !m_has_wait_wlock) {
        m_status = CORWMUTEX_RLOCKED;
        m_rlock_hold_num++;

        if (!m_has_wait_wlock)
            _NotifyAll(true);

        _SysUnLock();
        return 0;
    }

    int ret = _WaitRLockWithTimeout(ms, [this](){
        _SysUnLock();
        return true;
    });

    _SysLock();
    if (ret != 0) {
        _SysUnLock();
        return ret;
    }

    if (m_status == CORWMUTEX_WLOCKED || m_has_wait_wlock) {
        _SysUnLock();
        return -1;
    }

    m_status = CORWMUTEX_RLOCKED;
    m_rlock_hold_num++;

    if (!m_has_wait_wlock)
        _NotifyAll(true);

    _SysUnLock();
    return 0;
}

int CoRWMutex::TryWLock()
{
    _SysLock();
    if (m_status != CORWMUTEX_FREE) {
        _SysUnLock();
        return -1;
    }

    m_status = CORWMUTEX_WLOCKED;
    _SysUnLock();
    return 0;
}

int CoRWMutex::TryWLock(int ms)
{
    _SysLock();
    if (m_status == CORWMUTEX_FREE) {
        m_status = CORWMUTEX_WLOCKED;
        _SysUnLock();
        return 0;
    }

    m_has_wait_wlock = true;
    int ret = _WaitWLockWithTimeout(ms, [this](){
        _SysUnLock();
        return true;
    });

    _SysLock();
    if (ret != 0) {
        m_has_wait_wlock = false;
        _SysUnLock();
        return ret;
    }

    if (m_status != CORWMUTEX_FREE) {
        m_has_wait_wlock = false;
        _SysUnLock();
        return -1;
    }

    m_has_wait_wlock = false;
    m_status = CORWMUTEX_WLOCKED;
    _SysUnLock();
    return 0;
}

int CoRWMutex::_NotifyOne()
{
    while (!m_wait_writelock_queue.empty()) {
        auto waiter = m_wait_writelock_queue.front();
        m_wait_writelock_queue.pop();
        if (waiter->Notify() == 0)
            return 0;
    }

    while (!m_wait_readlock_queue.empty()) {
        auto waiter = m_wait_readlock_queue.front();
        m_wait_readlock_queue.pop();
        if (waiter->Notify() == 0)
            return 0;
    }

    return -1;
}

int CoRWMutex::_NotifyAll(bool reader)
{
    auto& notify_queue_ref = reader ? m_wait_readlock_queue : m_wait_writelock_queue;

    while (!notify_queue_ref.empty()) {
        auto waiter = notify_queue_ref.front();
        notify_queue_ref.pop();
        waiter->Notify();
    }

    return 0;
}


int CoRWMutex::_WaitRLock(detail::CoroutineOnYieldCallback&& cb)
{
    auto waiter = CoWaiter::Create();
    m_wait_readlock_queue.push(waiter);
    return waiter->WaitWithCallback(std::forward<detail::CoroutineOnYieldCallback&&>(cb));
}

int CoRWMutex::_WaitWLock(detail::CoroutineOnYieldCallback&& cb)
{
    auto waiter = CoWaiter::Create();
    m_wait_writelock_queue.push(waiter);
    return waiter->WaitWithCallback(std::forward<detail::CoroutineOnYieldCallback&&>(cb));
}

int CoRWMutex::_WaitRLockWithTimeout(int ms, detail::CoroutineOnYieldCallback&& cb)
{
    auto waiter = CoWaiter::Create();
    m_wait_readlock_queue.push(waiter);
    return waiter->WaitWithTimeoutAndCallback(ms, std::forward<detail::CoroutineOnYieldCallback&&>(cb));
}

int CoRWMutex::_WaitWLockWithTimeout(int ms, detail::CoroutineOnYieldCallback&& cb)
{
    auto waiter = CoWaiter::Create();
    m_wait_writelock_queue.push(waiter);
    return waiter->WaitWithTimeoutAndCallback(ms, std::forward<detail::CoroutineOnYieldCallback&&>(cb));
}

void CoRWMutex::_SysLock()
{
    m_mutex.lock();
}

void CoRWMutex::_SysUnLock()
{
    m_mutex.unlock();
}

}
