#include <bbt/coroutine/sync/CoRWMutex.hpp>
#include <bbt/coroutine/detail/DriverWakeDeferral.hpp>

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
            /* 等待登记回调仍持 SysLock：若回调里触发 Wake，推迟到本
             * lambda 退出（锁已释放）之后；guard 不跨 Yield。 */
            detail::DriverWakeDeferral _defer;
            _OnReaderBlocked();
            _SysUnLock();
            return true;
        });

        _SysLock();
    }

    /* 读锁获取成功 */
    {
        /* 最终非挂起段：Notify 链在 m_mutex 内执行，Wake 推迟到
         * _SysUnLock 之后；guard 不覆盖上方可能 Yield 的等待循环。 */
        detail::DriverWakeDeferral _defer;
        m_status = CORWMUTEX_RLOCKED;
        m_rlock_hold_num++;

        /* 如果有等待中的写锁，那么就不在唤醒读锁了 */
        if (!m_has_wait_wlock)
            _NotifyAll(true);

        _SysUnLock();
    }
    return 0;
}

int CoRWMutex::WLock()
{
    _SysLock();
    while (m_status != CORWMUTEX_FREE) {
        m_has_wait_wlock = true;
        _WaitWLock([this](){
            detail::DriverWakeDeferral _defer;   // 同 RLock：不跨 Yield
            _OnWriterQueued();
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
    detail::DriverWakeDeferral _defer;   // Notify 链 Wake 推迟到锁外
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
    detail::DriverWakeDeferral _defer;   // 同 RUnLock
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
    detail::DriverWakeDeferral _defer;   // 非挂起路径，可整体覆盖
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
    {
        detail::DriverWakeDeferral _defer;   // 首个非挂起段
        _SysLock();
        if (m_status != CORWMUTEX_WLOCKED && !m_has_wait_wlock) {
            m_status = CORWMUTEX_RLOCKED;
            m_rlock_hold_num++;

            if (!m_has_wait_wlock)
                _NotifyAll(true);

            _SysUnLock();
            return 0;
        }
        /* 未立即获锁：无 Notify 已发生，deferral 退出无 pending；
         * m_mutex 保持持有进入等待登记（锁协议不变）。 */
    }

    int ret = _WaitRLockWithTimeout(ms, [this](){
        detail::DriverWakeDeferral _defer;   // 不跨 Yield
        _OnReaderBlocked();
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

    {
        detail::DriverWakeDeferral _defer;   // 最终非挂起段
        m_status = CORWMUTEX_RLOCKED;
        m_rlock_hold_num++;

        if (!m_has_wait_wlock)
            _NotifyAll(true);

        _SysUnLock();
    }
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
        detail::DriverWakeDeferral _defer;   // 不跨 Yield
        _OnWriterQueued();
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

void CoRWMutex::_OnWriterQueued() noexcept
{
    if (m_writer_queued_callback == nullptr)
        return;

    try
    {
        m_writer_queued_callback();
    }
    catch (...)
    {
    }
}

void CoRWMutex::_OnReaderBlocked() noexcept
{
    if (m_reader_blocked_callback == nullptr)
        return;

    try
    {
        m_reader_blocked_callback();
    }
    catch (...)
    {
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
