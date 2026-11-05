/** @file CoCond.hpp
 * @brief 协程条件变量：等待者在队列里挂起，由 Notify 族唤醒。
 */
#pragma once
#include <queue>
#include <mutex>
#include <bbt/coroutine/sync/CoWaiter.hpp>

namespace bbt::coroutine::sync {

/**
 * @brief 协程条件变量：Wait 族挂起并入队，NotifyOne 唤醒一个、NotifyAll 唤醒全部。
 *        唤醒不携带状态、谓词须调用方复查；Wait 族仅协程内；析构只对残留等待者告警，不唤醒。
 */
class CoCond : public std::enable_shared_from_this<CoCond> {
    struct PrivateTag {};

public:
    typedef std::shared_ptr<CoCond> SPtr;

    /// @brief 创建条件变量。
    static SPtr Create();

    /// @brief 仅经 Create() 构造。
    BBTATTR_FUNC_CTOR_HIDDEN
    CoCond(PrivateTag);
    /// @brief 销毁；残留等待者只告警，不唤醒。
    virtual ~CoCond();

    /**
     * @brief 挂起当前协程，直到被 Notify 唤醒（只能在协程中使用）。
     * @return 0 被唤醒；-1 等待位已被占用或等待事件创建/登记失败。
     */
    int Wait();

    /**
     * @brief 挂起当前协程直到被 Notify 唤醒或超时 ms 毫秒（仅协程内）。
     * @return 0 被唤醒；1 超时（协程级取消也按 1 上报）；-1 失败。
     */
    int WaitFor(int ms);

    /**
     * @brief 唤醒一个等待者，按入队顺序跳过已取消/已触发的项。
     * @return 0 有等待者被唤醒；-1 没有可唤醒的等待者。
     */
    int NotifyOne();

    /**
     * @brief 唤醒队列中所有等待者；无等待者时直接返回。
     */
    void NotifyAll();

protected:
    int _NotifyOne();

private:
    void _SysLock()
    {
        m_mutex.lock();
    }
    void _SysUnLock()
    {
        m_mutex.unlock();
    }

    std::queue<std::shared_ptr<CoWaiter>> m_waiter_queue;
    std::mutex m_mutex;
};

} // namespace bbt::coroutine::sync
