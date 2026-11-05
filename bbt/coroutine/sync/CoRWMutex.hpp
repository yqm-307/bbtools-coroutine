/** @file CoRWMutex.hpp
 * @brief 协程读写锁：写者优先，等待者挂起而不是占住 worker 线程。
 */
#pragma once
#include <functional>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>

namespace bbt::coroutine::sync {

/**
 * @brief 协程读写锁（写者优先）：有写者排队时新读者拿不到读锁；最后一个读者释放才通知写者。
 *        RLock/WLock 与带超时 Try* 须在协程内；返回码 0/1/-1，等待准备失败抛异常时 m_mutex 已解开；不支持读锁升级（会互等）。
 */
class CoRWMutex {
    struct PrivateTag {};

public:
    friend class CoRWMutexTestAccess;
    typedef std::shared_ptr<CoRWMutex> SPtr;
    /// @brief 创建一把未锁定的读写锁。
    static SPtr Create();
    /// @brief 仅经 Create() 构造。
    BBTATTR_FUNC_CTOR_HIDDEN
    CoRWMutex(PrivateTag)
    {
    }
    /// @brief 销毁；不唤醒等待者，销毁前须保证没有协程在等待。
    ~CoRWMutex()
    {
    }

    /**
     * @brief 获取读锁；有写者持锁或有写者在排队时挂起当前协程。
     * @return 0 成功；-1 等待失败，未持锁。
     */
    int RLock();
    /**
     * @brief 获取写锁；锁非空闲时挂起当前协程。
     * @return 0 成功；-1 等待失败，未持锁。
     */
    int WLock();

    /// @brief 释放一个读锁；最后一个读者释放时唤醒等待者。
    int RUnLock();
    /// @brief 释放写锁：有写者排队时唤醒一个写者，否则唤醒全部读者。
    int WUnLock();

    /// @brief 立即尝试获取读锁，不挂起；有写者持锁或排队时返回 -1。
    int TryRLock();
    /// @brief 立即尝试获取写锁，不挂起；锁非空闲时返回 -1。
    int TryWLock();

    /**
     * @brief 在 ms 毫秒内尝试获取读锁；不能立即获锁且 ms<0 时返回 -1。
     * @return 0 成功；1 超时；-1 失败（含等待登记失败、唤醒后已被写者抢先）
     */
    int TryRLock(int ms);
    /**
     * @brief 在 ms 毫秒内尝试获取写锁；不能立即获锁且 ms<0 时返回 -1。
     * @return 0 成功；1 超时；-1 失败（含等待登记失败、唤醒后锁已被别人拿走）
     */
    int TryWLock(int ms);

protected:
    // 唤醒都在 m_mutex 内完成；被置空的 tombstone 槽位按 nullptr 跳过。
    int _NotifyOne();
    int _NotifyAll(bool reader);
    int _WaitRLock(detail::CoroutineOnYieldCallback&& cb);
    int _WaitWLock(detail::CoroutineOnYieldCallback&& cb);
    int _WaitRLockWithTimeout(int ms, detail::CoroutineOnYieldCallback&& cb);
    int _WaitWLockWithTimeout(int ms, detail::CoroutineOnYieldCallback&& cb);

    void _SysLock();
    void _SysUnLock();
    void _OnWriterQueued() noexcept;
    void _OnReaderBlocked() noexcept;

private:
    std::queue<std::shared_ptr<CoWaiter>> m_wait_readlock_queue;
    std::queue<std::shared_ptr<CoWaiter>> m_wait_writelock_queue;

    int m_rlock_hold_num{0};
    // 有写者排队时禁止新读者进入（写者优先）；值按 m_wait_wlock_num 重建。
    bool m_has_wait_wlock{false};
    /* 仍存活的等待写者计数。由挂起回调在持锁临界区内 +1，
     * 等待返回（唤醒/超时）后由本写者 -1；m_has_wait_wlock 按
     * (num>0) 重建——登记失败不等价于写者责任结束，不得无条件清旗
     * 掩盖其它真实排队写者。 */
    int m_wait_wlock_num{0};

    std::mutex m_mutex;
    // 由 m_mutex 保护；RLOCKED 时 m_rlock_hold_num 是当前读者数。
    CoRWMutexStatus m_status{CORWMUTEX_FREE};
    // 测试注入点：由 CoRWMutexTestAccess 在等待登记前后打点，生产路径保持空。
    std::function<void()> m_writer_queued_callback{nullptr};
    std::function<void()> m_reader_blocked_callback{nullptr};
};

} // bbt::coroutine::sync
