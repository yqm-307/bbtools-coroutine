#include <bbt/coroutine/sync/CoRWMutex.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

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
    while (m_status == CORWMUTEX_WLOCKED || _ShouldBlockReaderLocked()) {
        _WaitRLock([this](){
            _SysUnLock();
            return true;
        });

        _SysLock();
    }

    /* 读锁获取成功 */
    m_status = CORWMUTEX_RLOCKED;
    m_rlock_hold_num++;
    m_rlock_owners[g_bbt_tls_coroutine_co]++;

    /* 如果有等待中的写锁，那么就不在唤醒读锁了 */
    if (!_HasWaitingWriterLocked())
        _NotifyAll(true);

    _SysUnLock();
    return 0;
}

int CoRWMutex::WLock()
{
    _SysLock();
    while (m_status != CORWMUTEX_FREE) {
        _WaitWLock([&](){
            _SysUnLock();
            return true;
        });

        _SysLock();
    }

    m_status = CORWMUTEX_WLOCKED;
    m_wlock_owner = g_bbt_tls_coroutine_co;
    _SysUnLock();
    return 0;
}

int CoRWMutex::UnLock()
{
    _SysLock();

    /* 如果解锁前是被读锁持有，则根据读锁持有数量判断锁状态；如果解锁前是写锁被持有，则释放时是空闲状态 */
    if (m_status == CORWMUTEX_RLOCKED) {
        auto iter = m_rlock_owners.find(g_bbt_tls_coroutine_co);
        if (iter == m_rlock_owners.end() || iter->second <= 0) {
            _SysUnLock();
            return -1;
        }

        iter->second--;
        if (iter->second == 0)
            m_rlock_owners.erase(iter);

        /* 如果被读锁持有，那么就减少持有数，并根据以持有读锁的协程数来判断状态 */
        m_rlock_hold_num--;
        m_status = (m_rlock_hold_num == 0) ? CORWMUTEX_FREE : CORWMUTEX_RLOCKED;
    }
    else if (m_status == CORWMUTEX_WLOCKED) {
        if (m_wlock_owner != g_bbt_tls_coroutine_co)
        {
            _SysUnLock();
            return -1;
        }

        /* 如果此锁被写锁持有，写锁只能有一个协程持有，所以锁状态直接进入free */
        m_status = CORWMUTEX_FREE;
        m_wlock_owner = nullptr;
    }
    else {
        _SysUnLock();
        return -1;
    }

    _NotifyOne();

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
    while (!m_wait_writelock_queue.empty()) {
        auto waiter = m_wait_writelock_queue.front();
        m_wait_writelock_queue.pop();
        if (waiter->Notify() == 0)
            return 0;
    }

    /* 解锁一个读锁 */
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
    int notify_num = 0;
    while (!notify_queue_ref.empty()) {
        auto waiter = notify_queue_ref.front();
        notify_queue_ref.pop();
        if (waiter->Notify() == 0)
            notify_num++;
    }

    return notify_num;
}


int CoRWMutex::_WaitRLock(detail::CoroutineOnYieldCallback&& cb)
{
    auto waiter = CoWaiter::Create();
    return waiter->WaitWithCallback([this, waiter, cb]() {
        m_wait_readlock_queue.push(waiter);
        if (cb == nullptr)
            return true;

        return cb();
    });
}

int CoRWMutex::_WaitWLock(detail::CoroutineOnYieldCallback&& cb)
{
    auto waiter = CoWaiter::Create();
    return waiter->WaitWithCallback([this, waiter, cb]() {
        m_wait_writelock_queue.push(waiter);
        if (cb == nullptr)
            return true;

        return cb();
    });
}

bool CoRWMutex::_HasWaitingWriterLocked() const
{
    return !m_wait_writelock_queue.empty();
}

bool CoRWMutex::_ShouldBlockReaderLocked() const
{
    switch (kFairnessPolicy)
    {
    case FairnessPolicy::WriterPreferred:
        return _HasWaitingWriterLocked();
    default:
        return false;
    }
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
