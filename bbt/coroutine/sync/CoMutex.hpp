/** @file CoMutex.hpp
 * @brief 协程互斥锁：抢不到锁的协程挂起，不占住 worker 线程。
 */
#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/sync/interface/ICoLock.hpp>

namespace bbt::coroutine::sync {

/**
 * @brief 协程间互斥锁：抢不到锁的协程挂起；唤醒按 FIFO 但不移交锁，被唤醒者须重新竞争。
 *        Lock/TryLock(ms) 仅协程内，UnLock/TryLock() 不挂起；同协程重复 Lock 触发 "comutex deadlock!" 断言（NDEBUG 只打印）。
 */
class CoMutex : public ICoLock {
    struct PrivateTag {};

public:
    typedef std::shared_ptr<CoMutex> SPtr;
    /// @brief 创建一把未锁定的 CoMutex。
    static SPtr Create();

    /// @brief 仅经 Create() 构造，初始状态为未锁定。
    BBTATTR_FUNC_CTOR_HIDDEN
    CoMutex(PrivateTag);
    /// @brief 销毁。析构不唤醒等待者：销毁时仍在 Lock 上等待的协程不会再被恢复。
    ~CoMutex();

    /// @brief 获取锁；已被占用时挂起当前协程，直到有协程 UnLock 后重新抢到锁。
    void Lock();
    /// @brief 释放锁并唤醒一个等待者；唤醒不转移所有权，被唤醒者仍要重新竞争。
    void UnLock();

    /**
     * @brief 在 ms 毫秒内获取锁，无法立即获取时挂起当前协程；不能立即获锁且 ms<0 时返回 -1。
     * @return 0 成功；1 超时（协程级取消也按 1 上报）；-1 等待失败，未持锁。
     */
    int TryLock(int ms);

    /**
     * @brief 尝试获取锁，立即返回，不挂起。
     * @return 0 成功；-1 已被占用或触发死锁断言。
     */
    int TryLock();

protected:
    // 以下等待辅助函数都在调用方已持有 m_mutex 时进入；挂起回调负责在协程真正挂起后
    // 释放它，返回后由调用方重新 _SysLock（见 CoMutex.cc 的 cb_ran 协议）。
    int _WaitUnLockUnitlTimeout(int timeout, const detail::CoroutineOnYieldCallback& cb);
    int _WaitUnLock(const detail::CoroutineOnYieldCallback& cb);

    void _NotifyOne();

    void _SysLock();
    void _SysUnLock();

    void _OnLocked();
    void _OnUnLocked();

private:
    // 等待者按入队顺序排队；被置空的槽位是等待装配失败回滚的 tombstone，_NotifyOne 跳过。
    std::queue<std::shared_ptr<detail::CoPollEvent>> m_wait_event_queue;
    // 当前持锁协程，只用于同协程重复加锁的死锁断言，不参与所有权判定。
    detail::Coroutine::Ptr m_locked_co{nullptr};
    std::mutex m_mutex;
    volatile CoMutexStatus m_status{CoMutexStatus::COMUTEX_FREE};
};

} // bbt::coroutine::sync