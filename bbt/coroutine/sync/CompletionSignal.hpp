#pragma once
#include <chrono>
#include <memory>
#include <mutex>
#include <bbt/coroutine/object/interface/ICoObject.hpp>
#include <bbt/coroutine/sync/Cancellation.hpp>

namespace bbt::coroutine
{

namespace sync
{

class CoWaiter;

}

/**
 * @brief 等待截止时间：单调时钟（steady_clock）。
 *
 * WaitOptions 默认取 max() 表示不设截止；显式传入已过期的 deadline
 * 时 Wait 立即返回 TimedOut，不进行任何事件注册（契约 §C1）。
 * 已知上限：剩余期限达到 INT_MAX 毫秒（约 24.8 天）即不挂定时器，
 * 与无期限等价；该上限来自定时器的 int 毫秒口径，不做钳制。
 */
using Deadline = std::chrono::steady_clock::time_point;

enum class WaitStatus
{
    Completed,          ///< 信号已完成
    TimedOut,           ///< 本次等待到达 deadline
    Cancelled,          ///< options.cancel 或当前协程协作取消终止了等待
    InvalidContext,     ///< 非协程上下文调用 Wait
    AlreadyWaiting,     ///< 已有其它等待者占用唯一等待位
    RuntimeUnavailable, ///< Scheduler 未运行 / 已开始 Stop / 信号属于旧代际
};

struct WaitOptions
{
    Deadline            deadline = Deadline::max();
    CancellationToken   cancel{};
};

/**
 * @brief one-shot 完成信号：可提前完成、可安全晚到的等待桥（#347 契约 §C1）
 *
 * 语义要点：
 * - one-shot 不 reset：第一次 Complete() 返回 true，之后返回 false；
 *   完成先于 Wait 不丢通知，晚到 Wait 立即返回 Completed。
 * - 同一时刻至多一个等待者，并发第二个 Wait 返回 AlreadyWaiting；
 *   同一协程在超时/取消返回后可再次 Wait 同一信号。
 * - Wait 只允许在所属运行时代际的协程内调用；非协程上下文返回
 *   InvalidContext；Stop 中或旧代际信号一律 RuntimeUnavailable
 *   （即便已完成）——环境检查先于完成检查。
 * - #347③：挂起等待整体委托给组合的 CoWaiter 窄接口——胜负在同一
 *   竞争点（事件状态机首个胜出触发的唤醒原因掩码）一次定死，恢复后
 *   不做第二遍优先级重判：完成触发 → Completed，定时器超时 →
 *   TimedOut，协程/令牌取消 → Cancelled。入口检查顺序保持
 *   已完成 > 取消 > 超时 > 占用：m_completed 置位后新 Wait 直接
 *   决议 Completed，不再进入挂起。
 * - Complete 可晚于 Stop：只记录完成，不访问已被 Stop 回收的等待者。
 * - 只携带通知不携带业务 payload；上层须先写结果再 Complete
 *   （Complete 的发布与 Wait 的观察经同一互斥建立可见性）。
 *
 * 所有权归外部持有者（通常 shared_ptr）；Complete 可被已授权的任意
 * 线程调用，不绑定协程上下文。
 */
class CompletionSignal
{
public:
    /**
     * @brief 构造须处于运行中的运行时代际（普通线程或协程均可）。
     * @throw std::logic_error Scheduler 未运行或已开始 Stop（无代际可归属）。
     */
    CompletionSignal();
    CompletionSignal(const CompletionSignal&) = delete;
    CompletionSignal& operator=(const CompletionSignal&) = delete;

    bool                            Complete() noexcept;
    WaitStatus                      Wait(const WaitOptions& options);
private:
    std::mutex                      m_mtx;
    bool                            m_completed{false};
    /* 在途等待计数：仅在 m_completed 置位前进入窄等待的调用被计入；
     * Complete 据此决定是否需要经 CoWaiter::Notify 投递唤醒，以及
     * 重试的停止边界。m_completed 置位后新 Wait 在入口即返回，
     * 计数只减不增，投递自旋必然收敛。 */
    int                             m_waiting{0};
    /* 等待位占用与唤醒仲裁整体委托给 CoWaiter（#347③ 组合复用）：
     * 信号本体跨挂起状态不再持有底层 CoPollEvent——事件生命周期与
     * Stop 回收由 CoWaiter 及协程事件状态机路径承担，本对象没有可
     * 失效的事件引用。 */
    std::shared_ptr<sync::CoWaiter> m_waiter;
    RuntimeGeneration               m_generation{0};
};

} // namespace bbt::coroutine
