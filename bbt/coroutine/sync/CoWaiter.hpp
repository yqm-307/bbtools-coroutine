#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/sync/WaitTypes.hpp>
#include <bbt/coroutine/sync/CoEventValue.hpp>

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
     *    Cancelled          —— 协程级 RequestCancel（POLL_EVENT_CANCELLED）
     *    AlreadyWaiting     —— 已有其它等待者占用唯一等待位（前一事件已
     *                          到终态的残留不占位，避免与未恢复的占位者互饿）
     *    InvalidContext     —— 非协程上下文
     *    RuntimeUnavailable —— 事件创建/登记失败（未真正挂起）
     *  与 Wait 族不同：超时族旧入口对取消仍返回 1（兼容映射），本接口把取消
     *  与超时分开报告。Cancel() 语义不变——只是让 Notify 跳过，不是唤醒。
     * @param options deadline 用单调时钟
     */
    WaitStatus                          Wait(const WaitOptions& options);

    /**
     * @brief 窄接口 + 挂起后回调 + 载荷输出（带参等待）。
     *
     * 单次流程：准备等待 → 登记成功 → 调用一次 on_registered → 等待恢复。
     * on_registered 只在登记成功后执行（登记失败不投递，避免孤儿请求）；
     * 回调内做同步 Notify 走 PENDING 早到路径，不丢唤醒。
     * 结果口径同窄接口 Wait；额外把首个胜出 Notify 携带的载荷经 value 输出：
     *  - 以 POLL_EVENT_CUSTOM 首胜时输出该次 Notify 的载荷（无参 Notify 输出空值）；
     *  - 定时器/协程取消首胜时 value 置为空值，不返回败方载荷，也不残留上次结果；
     *  - value == nullptr 时与 Wait(options) 等价。
     * @param options deadline 用单调时钟
     * @param on_registered 挂起成功后的投递回调（可为空）
     * @param value 载荷输出，可空；指向对象由调用方保活
     */
    WaitStatus                          WaitWithCallback(
        const WaitOptions& options,
        const detail::CoroutineOnYieldCallback& on_registered,
        CoEventValue* value = nullptr);

    /**
     * @brief #346 组合等待（最小稳定切片）：FD interests、绝对 deadline、
     *  既有 Notify()（custom）与协程级 RequestCancel 的首胜等待，恢复后
     *  返回可区分结果，不做第二遍优先级重判：
     *    FdReadable / FdWriteable —— fd interest 就绪首胜（底层 EV_READ/EV_WRITE 位）
     *    Completed                —— Notify() 到达（POLL_EVENT_CUSTOM 位）
     *    TimedOut                 —— 真实定时器超时，或进入时已过期
     *    Cancelled                —— 协程级 RequestCancel
     *    AlreadyWaiting / InvalidContext / RuntimeUnavailable —— 同窄接口 Wait
     *    InvalidOptions           —— 申请 fd interest 但 fd < 0
     *  胜负仍由 CoPollEvent 状态机首个胜出 Trigger 锁定的 flags 定死。
     *  fd 触发位沿用 pollevent EventOpt 数值（READABLE=0x02/WRITEABLE=0x04），
     *  与 PollEventType 的可读/可写位数值互换，判定不得混用两套常量。
     * @param options fd 须为合法描述符（仅当申请 interest 时）；deadline 单调
     *  时钟，max() 表示不设截止。Notify() 复用既有等待位，本等待期间外部
     *  Notify() 照常生效。
     */
    CombinedWaitStatus                  Wait(const CombinedWaitOptions& options);
    CombinedWaitStatus                  Wait(
        const CombinedWaitOptions& options,
        const detail::CoroutineOnYieldCallback& on_registered);

    /**
     * @brief 组合等待 + 载荷输出：语义同上一重载，额外把首个胜出 Notify
     *  携带的载荷经 value 输出（非 custom 首胜置空值），口径同带参
     *  WaitWithCallback。
     */
    CombinedWaitStatus                  Wait(
        const CombinedWaitOptions& options,
        const detail::CoroutineOnYieldCallback& on_registered,
        CoEventValue* value);

    /**
     * @brief 唤醒一个因为调用Wait、WaitWithTimeout而挂起的协程
     *  ，如果没有携程因为Wait相关调用挂起，则Notify会失败
     * @return  0表示成功，-1表示事件已经触发
     */
    int                                 Notify();

    /**
     * @brief 带载荷唤醒：与 Notify() 同语义，额外把 value 随本次首胜交付。
     *
     * 无有效等待者（未 Wait / 已决议 / 已解绑）或已有通知在途时返回 -1，
     * 不保存载荷、不触发断言；输掉竞争的 Notify 不覆盖赢家载荷。载荷由
     * 带参 Wait/带参 WaitWithCallback 的输出参数交付，指针载荷只借用不延寿。
     * @return  0表示成功，-1表示无有效等待者
     */
    int                                 Notify(const CoEventValue& value);

    /**
     * @brief 标记此 Waiter 为已取消，Notify 将跳过。
     *
     * 注意：这不是唤醒入口，也不表达业务取消；业务取消由 adapter 经
     * 带载荷 Notify 表达，再由上层把 Completed 解释成 Cancelled/Closed。
     */
    void                                Cancel() { m_run_status = COND_FREE; }
protected:
    std::mutex                          m_notify_mutex;
    std::shared_ptr<detail::CoPollEvent> m_co_event{nullptr};
    volatile CoCondStatus               m_run_status{COND_DEFAULT};
    /* 载荷不放在本共享槽：首胜 Notify 的载荷落在本轮 CoPollEvent 上
     *（CoPollEvent::SetNotifyValue / GetNotifyValue），各轮等待互不覆写。 */
};

}
