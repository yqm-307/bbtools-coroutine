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
        _WaitWLock([&](){
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

int CoRWMutex::UnLock()
{
    _SysLock();

    _NotifyOne();

    /* 如果解锁前是被读锁持有，则根据读锁持有数量判断锁状态；如果解锁前是写锁被持有，则释放时是空闲状态 */
    if (m_status == CORWMUTEX_RLOCKED) {
        /* 如果被读锁持有，那么就减少持有数，并根据以持有读锁的协程数来判断状态 */
        m_rlock_hold_num--;
        m_status = (m_rlock_hold_num == 0) ? CORWMUTEX_FREE : CORWMUTEX_RLOCKED;
    }
    else if (m_status == CORWMUTEX_WLOCKED) {
        /* 如果此锁被写锁持有，写锁只能有一个协程持有，所以锁状态直接进入free */
        m_status = CORWMUTEX_FREE;
    }

    _SysUnLock();
    return 0;
}

int CoRWMutex::_NotifyOne()
{
    /**
     * 读写锁使用一般在读 “远大于” 写的情况使用
     * 所以实现读写锁自然是假设读操作频率远高于写操作
     * 
     * 为了防止“写饿死”
     * 
     */

    /* 解锁一个写锁 */
    if (!m_wait_writelock_queue.empty()) {
        auto waiter = m_wait_writelock_queue.front();
        m_wait_writelock_queue.pop();
        waiter->Notify();
        return 0;
    }

    /* 解锁一个读锁 */
    if (!m_wait_readlock_queue.empty()) {
        auto waiter = m_wait_readlock_queue.front();
        m_wait_readlock_queue.pop();
        waiter->Notify();
        return 0;
    }

    return -1;
}

int CoRWMutex::_NotifyAll(bool reader)
{
    auto& notify_queue_ref = reader ? m_wait_readlock_queue : m_wait_writelock_queue; 
    if (notify_queue_ref.empty())
        return 0;

    /* 唤醒所有等待的协程 */
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

void CoRWMutex::_SysLock()
{
    m_mutex.lock();
}

void CoRWMutex::_SysUnLock()
{
    m_mutex.unlock();
}

}