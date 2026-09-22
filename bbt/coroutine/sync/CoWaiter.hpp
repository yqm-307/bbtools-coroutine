#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/sync/CompletionSignal.hpp>

namespace bbt::coroutine::sync
{

/**
 * @brief 实现协程等待和唤醒的功能
 * 
 * 通过CoWaiter可以定制的去实现协程间的同步机制，事实上
 * bbtco中也是如此做的
 */
class CoWaiter
{
public:
    typedef std::shared_ptr<CoWaiter> SPtr;
    static SPtr                         Create();

    CoWaiter();
    ~CoWaiter();

    /**
     * @brief 挂起当前协程，直到被唤醒。如果有多个协程调用Wait族函数只有第一个成功
     *  其余调用者会失败。
     * @return 0表示被唤醒，-1表示失败
     */
    int                                 Wait();

    /**
     * @brief 挂起当前协程，知道被唤醒。如果有多个协程调用Wait族函数只有第一个成功
     *  其余调用者会失败。
     *  当参数cb为nullptr时，行为和Wait一致。和Wait同属Wait族函数
     * @param cb 当协程完成后调用此函数
     * @return 0表示被唤醒，-1表示失败
     */
    int                                 WaitWithCallback(const detail::CoroutineOnYieldCallback& cb);

    /**
     * @brief 挂起当前协程，直到被唤醒或者超时。如果有多个
     * 调用，只有第一个成功，其余调用者会失败
     * @param ms 
     * @return int 0表示触发事件，-1表示失败，1表示超时
     */
    int                                 WaitWithTimeout(int ms);

    /**
     * @brief 挂起当前协程，直到被唤醒或者超时。如果有多个
     * 调用，只有第一个成功，其余调用者失败
     * @param ms 
     * @param cb 
     * @return int 
     */
    int                                 WaitWithTimeoutAndCallback(int ms, const detail::CoroutineOnYieldCallback& cb);

    /**
     * @brief 窄接口（#347②）：一次等待返回可区分的 WaitStatus，
     *  结果由仲裁点（本次唤醒原因掩码）直接决定，恢复后不做第二遍优先级重判：
     *    Completed          —— Notify() 到达（POLL_EVENT_CUSTOM）
     *    TimedOut           —— 真实定时器超时（POLL_EVENT_TIMEOUT），或进入时已过期
     *    Cancelled          —— 协程级 RequestCancel 或 options.cancel 令牌（POLL_EVENT_CANCELLED）
     *    AlreadyWaiting     —— 已有其它等待者占用唯一等待位（前一事件已
     *                          到终态的残留不占位，避免与未恢复的占位者互饿）
     *    InvalidContext     —— 非协程上下文
     *    RuntimeUnavailable —— 事件创建/登记失败（未真正挂起）
     *  与 Wait 族不同：超时族旧入口对取消仍返回 1（兼容映射），本接口把取消
     *  与超时分开报告。Cancel() 语义不变——只是让 Notify 跳过，不是唤醒。
     * @param options deadline 用单调时钟；cancel 为取消令牌（登记唤醒目标，
     *  已取消的令牌由登记路径同步触发取消唤醒，仍经掩码仲裁）
     */
    WaitStatus                          Wait(const WaitOptions& options);

    /**
     * @brief 唤醒一个因为调用Wait、WaitWithTimeout而挂起的协程
     *  ，如果没有携程因为Wait相关调用挂起，则Notify会失败
     * @return  0表示成功，-1表示事件已经触发
     */
    int                                 Notify();

    /**
     * @brief 标记此 Waiter 为已取消，Notify 将跳过
     */
    void                                Cancel() { m_run_status = COND_FREE; }
protected:
    std::mutex                          m_notify_mutex;
    std::shared_ptr<detail::CoPollEvent> m_co_event{nullptr};
    volatile CoCondStatus               m_run_status{COND_DEFAULT};
};

}