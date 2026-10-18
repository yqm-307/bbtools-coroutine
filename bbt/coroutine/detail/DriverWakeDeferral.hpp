#pragma once
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail
{

/**
 * @brief driver Wake 延期机制（runtime 内部，无对外 API）。
 *
 * 触发链（CoWaiter::Notify → Trigger → OnActiveCoroutine → 广播）可能
 * 运行在同步原语外层 OS 锁内，而 driver->Wake() 必须在整条调用链的
 * runtime 锁之外执行。本机制提供 TLS 计数：
 *  - 构造 depth++，析构 depth--；最外层退出时若 pending 置位，清位后
 *    对全部已登记 worker 补发 driver Wake（不重新发布任务/latch）。
 *  - 延期期间任务发布与 latch 照常立即生效，只推迟外部 Wake 调用。
 *
 * 时序约束：guard 对象绝不能跨 Yield/协程挂起——TLS 属线程而非协程，
 * 跨挂起会把「外调窗口」错误延伸到恢复之后的其它上下文。
 */
class DriverWakeDeferral
{
public:
    DriverWakeDeferral() noexcept { ++_TlsDepth(); }
    ~DriverWakeDeferral() noexcept;

    DriverWakeDeferral(const DriverWakeDeferral&) = delete;
    DriverWakeDeferral& operator=(const DriverWakeDeferral&) = delete;

    /**
     * @brief 请求一次 driver Wake。深度>0 时只置位 pending 并返回 true
     *  （由最外层 guard 退出时统一兑现）；深度=0 返回 false，调用方自行
     *  立即执行 Wake。
     */
    static bool                     Defer() noexcept;
    /* 当前线程是否仍在某级 runtime 通知临界区内（deferral depth>0）。
     * 用于 Unregister 在副作用前拒绝穿透 guard 的终止 Wake。 */
    static bool                     IsActive() noexcept { return _TlsDepth() > 0; }
private:
    static int&                     _TlsDepth() noexcept;
    static bool&                    _TlsPending() noexcept;
};

}
