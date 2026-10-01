#pragma once
#include <chrono>

namespace bbt::coroutine
{

/**
 * @brief 等待截止时间：单调时钟（steady_clock）。
 *
 * WaitOptions 默认取 max() 表示不设截止；显式传入已过期的 deadline
 * 时 Wait 立即返回 TimedOut，不进行任何事件注册（契约 §C1）。
 * 已知上限：剩余期限达到 INT_MAX 毫秒（约 24.8 天）即不挂定时器，
 * 与无期限等价；该上限来自定时器的 int 毫秒口径，不做钳制。
 */
using Deadline = std::chrono::steady_clock::time_point;

/**
 * @brief 窄接口单次等待结果（协程级首胜原因）。
 *
 * 取消只来自协程级 RequestCancel；运行时不设业务取消令牌，业务取消由
 * 调用方经带载荷的 Notify 表达，再由上层把 Completed 解释为业务终态。
 */
enum class WaitStatus
{
    Completed,          ///< 信号已完成（带载荷 Notify 或纯 Notify 到达）
    TimedOut,           ///< 本次等待到达 deadline
    Cancelled,          ///< 当前协程协作取消终止了等待
    InvalidContext,     ///< 非协程上下文调用 Wait
    AlreadyWaiting,     ///< 已有其它等待者占用唯一等待位
    RuntimeUnavailable, ///< 事件建立/登记失败（等待基础设施不可用）
};

/**
 * @brief 窄接口等待选项。
 *
 * 不携带取消令牌：等待中的取消由协程级 RequestCancel 产生
 * WaitStatus::Cancelled，业务级取消改用带载荷 Notify。
 */
struct WaitOptions
{
    Deadline            deadline = Deadline::max();
};

} // namespace bbt::coroutine

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
    Cancelled,          ///< 协程级 RequestCancel 取消
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
 * 单调时钟，默认 max() 表示不设截止。不携带取消令牌：等待中的取消由
 * 协程级 RequestCancel 产生，业务级取消改用带载荷 Notify。
 */
struct CombinedWaitOptions
{
    bool                want_readable{false};   ///< 申请可读 interest
    bool                want_writeable{false};  ///< 申请可写 interest
    int                 fd{-1};                 ///< 等待的描述符
    Deadline            deadline = Deadline::max();
};

} // namespace bbt::coroutine::sync