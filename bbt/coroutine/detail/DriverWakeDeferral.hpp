#pragma once
#include <memory>
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail {

class CoPollEvent;
class WaitNode;

/**
 * @brief 同步原语通知的锁外收尾（runtime 内部）。
 *
 * 自定义通知在锁内竞争首胜，已挂起事件的完成与 driver Wake 留到最外层
 * 通知锁释放后执行。其它任务的发布和 latch 不延期，只推迟 driver Wake。
 *
 * guard 必须在所保护的锁释放后析构，且不能跨 Yield：TLS 属于线程而非协程。
 */
class DriverWakeDeferral {
public:
    DriverWakeDeferral() noexcept
    {
        auto& outermost = _TlsOutermost();
        if (outermost == nullptr)
            outermost = this;
    }
    ~DriverWakeDeferral() noexcept;

    DriverWakeDeferral(const DriverWakeDeferral&) = delete;
    DriverWakeDeferral& operator=(const DriverWakeDeferral&) = delete;

    /**
     * @brief 在 guard 内记住 Wake 请求，返回 true；否则由调用方立即 Wake。
     * 无目标为广播；同一目标合并，不同目标提升为一次广播，完成路径不分配。
     */
    static bool Defer(ProcesserId target = BBT_COROUTINE_INVALID_PROCESSER_ID) noexcept;

    /**
     * @brief 将已取得完成权的事件交给最外层 guard；无 guard 则由调用方完成。
     */
    static bool DeferCompletion(CoPollEvent& event) noexcept;
    static bool DeferCompletion(WaitNode& node) noexcept;

    /**
     * 当前线程是否仍在某级 runtime 通知临界区内。
     * 用于 Unregister 在副作用前拒绝穿透 guard 的终止 Wake。
     */
    static bool IsActive() noexcept { return _TlsOutermost() != nullptr; }

private:
    static DriverWakeDeferral*& _TlsOutermost() noexcept;

    std::shared_ptr<CoPollEvent> m_first_event;
    std::shared_ptr<CoPollEvent>* m_event_tail{&m_first_event};
    std::shared_ptr<WaitNode> m_first_wait_node;
    std::shared_ptr<WaitNode>* m_wait_node_tail{&m_first_wait_node};
};

}
