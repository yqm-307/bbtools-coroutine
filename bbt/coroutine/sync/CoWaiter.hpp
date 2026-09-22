#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/sync/CompletionSignal.hpp>

namespace bbt::coroutine::sync
{

/**
 * @brief #346 组合等待结果：首胜原因可区分（同窄接口 WaitStatus 的扩展口径）
 */
enum class CombinedWaitStatus
{
    FdReadable,         ///< fd 可读就绪首胜
    FdWriteable,        ///< fd 可写就绪首胜
    Completed,          ///< Notify() 到达
    TimedOut,           ///< deadline 到点（或进入时已过期）
    Cancelled,          ///< 协程级 RequestCancel 或 cancel 令牌取消
    AlreadyWaiting,     ///< 已有其它等待者占用唯一等待位
    InvalidContext,     ///< 非协程上下文
    InvalidOptions,     ///< 申请 fd interest 但 fd < 0
    RuntimeUnavailable, ///< 事件创建/登记失败（未真正挂起）
};

/**
 * @brief #346 组合等待选项
 *
 * interest 可独立或同时申请 READABLE / WRITEABLE，二者皆空表示纯信号
 * 等待；fd 仅在申请 interest 时使用，其余调用者不得依赖其值。deadline
 * 单调时钟，默认 max() 表示不设截止。
 */
struct CombinedWaitOptions
{
    bool                want_readable{false};   ///< 申请可读 interest
    bool                want_writeable{false};  ///< 申请可写 interest
    int                 fd{-1};                 ///< 等待的描述符
    Deadline            deadline = Deadline::max();
    CancellationToken   cancel{};
};

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
     * @brief #346 组合等待（最小稳定切片）：FD interests、绝对 deadline、
     *  既有 Notify()（custom）与 CancellationToken/协程级 RequestCancel 的
     *  首胜等待，恢复后返回可区分结果，不做第二遍优先级重判：
     *    FdReadable / FdWriteable —— fd interest 就绪首胜（底层 EV_READ/EV_WRITE 位）
     *    Completed                —— Notify() 到达（POLL_EVENT_CUSTOM 位）
     *    TimedOut                 —— 真实定时器超时，或进入时已过期
     *    Cancelled                —— 协程级 RequestCancel 或 options.cancel 令牌
     *    AlreadyWaiting / InvalidContext / RuntimeUnavailable —— 同窄接口 Wait
     *    InvalidOptions           —— 申请 fd interest 但 fd < 0
     *  胜负仍由 CoPollEvent 状态机首个胜出 Trigger 锁定的 flags 定死。
     *  fd 触发位沿用 pollevent EventOpt 数值（READABLE=0x02/WRITEABLE=0x04），
     *  与 PollEventType 的可读/可写位数值互换，判定不得混用两套常量。
     * @param options fd 须为合法描述符（仅当申请 interest 时）；deadline 单调
     *  时钟，max() 表示不设截止；cancel 为取消令牌（登记唤醒目标，已取消的
     *  令牌由登记路径同步触发取消唤醒，仍经掩码仲裁）。Notify() 复用既有
     *  等待位，本等待期间外部 Notify() 照常生效。
     */
    CombinedWaitStatus                  Wait(const CombinedWaitOptions& options);

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