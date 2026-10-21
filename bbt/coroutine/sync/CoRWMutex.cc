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
        /* #368：等待失败时挂起回调未执行，m_mutex 仍由本线程持有，
         * 直接由下方唯一出口解锁并返回 -1；不得重入 _SysLock。
         * #368-R1/F3：等待准备（CoWaiter 构造/排队/RegistCustom/回调装配）
         * 的一次分配失败会把异常抛出本函数——此时 m_mutex 仍持有，catch 里
         * 由 cb_ran 严格区分「回调已解锁」与「回调未执行」，不得双解锁
         * 或把锁残留带走。 */
        bool cb_ran = false;
        int wait_ret = -1;
        try {
            wait_ret = _WaitRLock([this, &cb_ran](){
                /* 等待登记回调仍持 SysLock：若回调里触发 Wake，推迟到本
                 * lambda 退出（锁已释放）之后；guard 不跨 Yield。 */
                detail::DriverWakeDeferral _defer;
                _OnReaderBlocked();
                _SysUnLock();
                cb_ran = true;
                return true;
            });
        } catch (...) {
            if (!cb_ran)
                _SysUnLock();
            throw;
        }
        if (wait_ret != 0) {
            _SysUnLock();
            return -1;
        }

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
        /* #368：失败补偿与 RLock 同——等待未挂起时锁仍在手，
         * 统一解锁返回 -1。#368-R1/F2：写者责任改为计数——
         * 挂起回调在持锁临界区 +1，等待返回后由本写者 -1 并按
         * 计数重建 m_has_wait_wlock；先挂起再登记失败的写者不残留
         * 责任，也不掩盖其它仍存活排队写者。
         * #368-R1/F3：等待准备抛异常时 cb_ran 区分锁归属；回调已
         * 完整执行（含解锁与计数）的极端路径重取锁结清计数再抛出。 */
        bool cb_ran = false;
        int wait_ret = -1;
        try {
            wait_ret = _WaitWLock([this, &cb_ran](){
                detail::DriverWakeDeferral _defer;   // 同 RLock：不跨 Yield
                _OnWriterQueued();
                ++m_wait_wlock_num;
                m_has_wait_wlock = true;
                _SysUnLock();
                cb_ran = true;
                return true;
            });
        } catch (...) {
            if (cb_ran) {
                _SysLock();
                --m_wait_wlock_num;
                m_has_wait_wlock = (m_wait_wlock_num > 0);
            }
            _SysUnLock();
            throw;
        }
        if (wait_ret == -1) {
            _SysUnLock();
            return -1;
        }

        _SysLock();
        /* 回调已执行（唤醒或超时恢复）：本写者排队责任到此结束，
         * 按仍存活等待写者重建标志。 */
        --m_wait_wlock_num;
        m_has_wait_wlock = (m_wait_wlock_num > 0);
    }

    m_has_wait_wlock = (m_wait_wlock_num > 0);
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

    bool cb_ran = false;
    int ret = -1;
    try {
        ret = _WaitRLockWithTimeout(ms, [this, &cb_ran](){
            detail::DriverWakeDeferral _defer;   // 不跨 Yield
            _OnReaderBlocked();
            _SysUnLock();
            cb_ran = true;
            return true;
        });
    } catch (...) {
        /* #368-R1/F3：同 RLock——异常按回调是否已解锁区分。 */
        if (!cb_ran)
            _SysUnLock();
        throw;
    }

    /* #368：失败路径挂起回调未执行，锁仍在手不得重入；
     * 成功/超时路径回调已释放锁，必须重取后再看 m_status。 */
    if (bbt_unlikely(ret == -1)) {
        _SysUnLock();
        return -1;
    }

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

    /* m_has_wait_wlock 的写者责任与 WLock 同一计数协议：挂起回调 +1，
     * 等待返回（ret!=-1，回调已执行）后 -1 重建；登记失败（-1）回调未
     * 执行不留责任；超时/被抢路径不再无条件清 false。 */
    bool cb_ran = false;
    int ret = -1;
    try {
        ret = _WaitWLockWithTimeout(ms, [this, &cb_ran](){
            detail::DriverWakeDeferral _defer;   // 不跨 Yield
            _OnWriterQueued();
            ++m_wait_wlock_num;
            m_has_wait_wlock = true;
            _SysUnLock();
            cb_ran = true;
            return true;
        });
    } catch (...) {
        if (cb_ran) {
            _SysLock();
            --m_wait_wlock_num;
            m_has_wait_wlock = (m_wait_wlock_num > 0);
        }
        _SysUnLock();
        throw;
    }

    /* #368：失败路径锁仍在手不得重入；同 RLock 失败补偿。 */
    if (bbt_unlikely(ret == -1)) {
        _SysUnLock();
        return -1;
    }

    _SysLock();
    --m_wait_wlock_num;
    m_has_wait_wlock = (m_wait_wlock_num > 0);
    if (ret != 0) {
        _SysUnLock();
        return ret;
    }

    if (m_status != CORWMUTEX_FREE) {
        _SysUnLock();
        return -1;
    }

    m_status = CORWMUTEX_WLOCKED;
    _SysUnLock();
    return 0;
}

int CoRWMutex::_NotifyOne()
{
    /* tombstone（装配失败回滚置空的槽位）不占唤醒名额、不阻断扫描——
     * 与 Notify() 返回 -1 的已死 waiter 同一消费语义。 */
    while (!m_wait_writelock_queue.empty()) {
        auto waiter = m_wait_writelock_queue.front();
        m_wait_writelock_queue.pop();
        if (waiter == nullptr)
            continue;
        if (waiter->Notify() == 0)
            return 0;
    }

    while (!m_wait_readlock_queue.empty()) {
        auto waiter = m_wait_readlock_queue.front();
        m_wait_readlock_queue.pop();
        if (waiter == nullptr)
            continue;
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
        if (waiter == nullptr)
            continue;
        waiter->Notify();
    }

    return 0;
}


int CoRWMutex::_WaitRLock(detail::CoroutineOnYieldCallback&& cb)
{
    auto waiter = CoWaiter::Create();
    m_wait_readlock_queue.push(waiter);
    /* #368-R2/F3：writer 优先在 _NotifyOne/_NotifyAll 的消费顺序里维持，
     * 不在持锁路径反复扫描队列；装配失败回滚以 tombstone 表达。 */
    try {
        return waiter->WaitWithCallback(std::forward<detail::CoroutineOnYieldCallback>(cb));
    } catch (...) {
        if (!m_wait_readlock_queue.empty() && m_wait_readlock_queue.back() == waiter)
            m_wait_readlock_queue.back().reset();
        throw;
    }
}

int CoRWMutex::_WaitWLock(detail::CoroutineOnYieldCallback&& cb)
{
    auto waiter = CoWaiter::Create();
    m_wait_writelock_queue.push(waiter);
    try {
        return waiter->WaitWithCallback(std::forward<detail::CoroutineOnYieldCallback>(cb));
    } catch (...) {
        if (!m_wait_writelock_queue.empty() && m_wait_writelock_queue.back() == waiter)
            m_wait_writelock_queue.back().reset();
        throw;
    }
}

int CoRWMutex::_WaitRLockWithTimeout(int ms, detail::CoroutineOnYieldCallback&& cb)
{
    /* #368：负期限的定时事件无法建立（RegistCustom→InitFdEvent 拒收），
     * 入队等待会永久悬挂且无唤醒源；按无效期限立即报错。 */
    if (ms < 0)
        return -1;
    auto waiter = CoWaiter::Create();
    m_wait_readlock_queue.push(waiter);
    try {
        return waiter->WaitWithTimeoutAndCallback(ms, std::forward<detail::CoroutineOnYieldCallback>(cb));
    } catch (...) {
        if (!m_wait_readlock_queue.empty() && m_wait_readlock_queue.back() == waiter)
            m_wait_readlock_queue.back().reset();
        throw;
    }
}

int CoRWMutex::_WaitWLockWithTimeout(int ms, detail::CoroutineOnYieldCallback&& cb)
{
    /* 同 _WaitRLockWithTimeout：负期限等待无唤醒源。 */
    if (ms < 0)
        return -1;
    auto waiter = CoWaiter::Create();
    m_wait_writelock_queue.push(waiter);
    try {
        return waiter->WaitWithTimeoutAndCallback(ms, std::forward<detail::CoroutineOnYieldCallback>(cb));
    } catch (...) {
        if (!m_wait_writelock_queue.empty() && m_wait_writelock_queue.back() == waiter)
            m_wait_writelock_queue.back().reset();
        throw;
    }
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
