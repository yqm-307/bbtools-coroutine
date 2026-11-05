/** @file CoWaiter.hpp
 * @brief 协程等待、唤醒与组合事件等待接口。
 */
#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/sync/WaitTypes.hpp>
#include <bbt/coroutine/sync/CoEventValue.hpp>

namespace bbt::coroutine::sync {

/**
 * @brief 单个等待位上的协程等待/唤醒原语。
 *
 * 一个 Waiter 同时只容纳一个等待者：Wait 族重复进入会失败，Notify 是「尝试
 * 唤醒」而非计数信号。Chan、CoCond、CoSelect 都建立在它之上。
 */
class CoWaiter {
public:
    typedef std::shared_ptr<CoWaiter> SPtr;
    /// @brief 创建一个空闲 Waiter（无等待者、未决议）。
    static SPtr Create();

    /// @brief 创建空闲 Waiter：无等待者、未决议。
    CoWaiter();
    /// @brief 销毁 Waiter。
    ~CoWaiter();

    /**
     * @brief 挂起当前协程，直到此 Waiter 被通知。
     * @return 被唤醒返回 0；等待位已占用或事件创建失败返回 -1。
     */
    int Wait();

    /**
     * @brief 挂起当前协程，直到被唤醒。如果有多个协程调用Wait族函数只有第一个成功
     *  其余调用者会失败。和 Wait 同属 Wait 族，占用同一个等待位。
     * @param cb 协程挂起成功后、恢复前调用一次；不可为空，需要无回调等待请用 Wait()
     * @return 0表示被唤醒，-1表示失败
     */
    int WaitWithCallback(const detail::CoroutineOnYieldCallback& cb);

    /**
     * @brief 挂起当前协程，直到被唤醒或者超时。如果有多个
     * 调用，只有第一个成功，其余调用者会失败
     * @param ms 超时毫秒
     * @return int 0表示触发事件，-1表示失败，1表示超时或协程级取消（兼容映射）
     */
    int WaitWithTimeout(int ms);

    /**
     * @brief 挂起当前协程，直到被唤醒或者超时。如果有多个
     * 调用，只有第一个成功，其余调用者失败
     * @param ms 超时毫秒
     * @param cb 协程挂起成功后、恢复前调用一次；不可为空
     * @return int 0表示触发事件，-1表示失败，1表示超时或协程级取消（兼容映射）
     */
    int WaitWithTimeoutAndCallback(int ms, const detail::CoroutineOnYieldCallback& cb);

    /**
     * @brief 窄接口：一次等待返回可区分的 WaitStatus。
     *
     * 仲裁点按唤醒原因掩码决定结果，恢复后不做第二遍优先级重判：
     * - Completed：Notify() 到达（POLL_EVENT_CUSTOM）
     * - TimedOut：真实定时器超时，或进入时已过期
     * - Cancelled：协程级 RequestCancel（POLL_EVENT_CANCELLED）
     * - AlreadyWaiting：唯一等待位已被非终态等待者占用
     * - InvalidContext：当前不在协程上下文
     * - RuntimeUnavailable：事件创建/登记失败（未真正挂起）
     *
     * WaitWithTimeout 对协程级取消仍返回 1；本接口将取消与超时分开报告。
     * Cancel() 只让 Notify 跳过，不会唤醒等待者。
     * @param options deadline 使用单调时钟
     * @return 首胜原因对应的 WaitStatus
     */
    WaitStatus Wait(const WaitOptions& options);

    /**
     * @brief 带挂起后回调与载荷输出的状态等待。
     *
     * 单次流程：准备等待 → 登记成功 → 调用一次 on_registered → 等待恢复。
     * on_registered 只在登记成功后执行（登记失败不投递，避免孤儿请求）；
     * 回调内做同步 Notify 走 PENDING 早到路径，不丢唤醒。
     * 结果口径同 `Wait(const WaitOptions&)`；额外把首个胜出 Notify 携带的载荷经 value 输出：
     * - POLL_EVENT_CUSTOM 首胜时输出该次 Notify 的载荷（无参 Notify 输出空值）；
     * - 定时器/协程取消首胜时 value 置为空值，不返回败方载荷，也不残留上次结果；
     * - value == nullptr 时与 `Wait(options)` 等价。
     * @param options deadline 使用单调时钟
     * @param on_registered 挂起成功后的投递回调，可为空
     * @param value 载荷输出，可空；指向对象由调用方保活
     */
    WaitStatus WaitWithCallback(const WaitOptions& options, const detail::CoroutineOnYieldCallback& on_registered,
                                CoEventValue* value = nullptr);

    /**
     * @brief 等待 fd、Notify、deadline 或协程取消中的首个事件。
     *
     * 恢复后按首个胜出 Trigger 锁存的 flags 返回，不做第二遍优先级重判。
     * 唤醒原因掩码在恢复入口已统一归一化为 PollEventType 位（fd 就绪位
     * 与底层 EventOpt 同名位值互换），决议只读归一化口径。
     * @param options fd interest 与 deadline 设置；deadline 使用单调时钟
     * @return 命中的组合等待状态
     */
    CombinedWaitStatus Wait(const CombinedWaitOptions& options);
    /**
     * @brief 挂起成功后、恢复前调用回调的组合等待重载。
     * @param options fd interest 与 deadline 设置
     * @param on_registered 挂起成功后的回调
     * @return 命中的组合等待状态
     */
    CombinedWaitStatus Wait(const CombinedWaitOptions& options, const detail::CoroutineOnYieldCallback& on_registered);

    /**
     * @brief 组合等待并输出首个胜出 Notify 的载荷。
     *
     * 非 custom 首胜时输出空值；指针载荷只借用，不延长对象寿命。
     * @param options fd interest 与 deadline 设置
     * @param on_registered 挂起成功后的回调，可为空
     * @param value 首胜 Notify 的载荷输出，可为空
     * @return 命中的组合等待状态
     */
    CombinedWaitStatus Wait(const CombinedWaitOptions& options, const detail::CoroutineOnYieldCallback& on_registered,
                            CoEventValue* value);

    /**
     * @brief 尝试唤醒当前等待者；没有有效等待者时不保存通知状态。
     *
     * fresh、已取消或已决议的 Waiter 返回 -1。先通知后等待的丢唤醒窗口
     * 由调用方持有的持久完成谓词关闭（登记后复查）。
     * @return  0表示成功，-1表示无有效等待者或事件已经触发
     */
    int Notify();

    /**
     * @brief 带载荷唤醒：与 Notify() 同语义，额外把 value 随本次首胜交付。
     *
     * 无有效等待者（未 Wait / 已决议 / 已解绑）或已有通知在途时返回 -1，
     * 不保存载荷、不触发断言；输掉竞争的 Notify 不覆盖赢家载荷。载荷由
     * 带参 Wait/带参 WaitWithCallback 的输出参数交付，指针载荷只借用不延寿。
     * @param value 与本次通知关联的载荷
     * @return  0表示成功，-1表示无有效等待者
     */
    int Notify(const CoEventValue& value);

    /**
     * @brief 标记此 Waiter 为已取消，使后续 Notify 跳过；不唤醒当前等待者。
     *
     * 这不表达业务取消；业务取消由 adapter 经带载荷 Notify 表达，再由上层
     * 把 Completed 解释成 Cancelled/Closed。
     */
    void Cancel()
    {
        m_run_status = COND_FREE;
    }

protected:
    std::mutex m_notify_mutex;
    std::shared_ptr<detail::CoPollEvent> m_co_event{nullptr};
    volatile CoCondStatus m_run_status{COND_DEFAULT};
    /* 载荷不放在本共享槽：首胜 Notify 的载荷落在本轮 CoPollEvent 上
     *（CoPollEvent::SetNotifyValue / GetNotifyValue），各轮等待互不覆写。 */
};

}
