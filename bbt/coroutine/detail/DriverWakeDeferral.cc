#include <bbt/coroutine/detail/DriverWakeDeferral.hpp>
#include <utility>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/WaitNode.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

namespace bbt::coroutine::detail {

namespace {

struct PendingWake {
    bool m_pending{false};
    ProcesserId m_target{BBT_COROUTINE_INVALID_PROCESSER_ID};
};

thread_local PendingWake tl_pending;

} // namespace

DriverWakeDeferral*& DriverWakeDeferral::_TlsOutermost() noexcept
{
    static thread_local DriverWakeDeferral* tl_outermost = nullptr;
    return tl_outermost;
}

bool DriverWakeDeferral::Defer(ProcesserId target) noexcept
{
    if (!IsActive())
        return false;

    if (!tl_pending.m_pending)
        tl_pending = {true, target};
    else if (tl_pending.m_target != target)
        tl_pending.m_target = BBT_COROUTINE_INVALID_PROCESSER_ID;
    return true;
}

bool DriverWakeDeferral::DeferCompletion(CoPollEvent& event) noexcept
{
    auto* outermost = _TlsOutermost();
    if (outermost == nullptr)
        return false;

    /**
     * 首胜线程独占事件的链接字段；队列持有寿命，入队不分配且保持完成顺序。
     */
    *outermost->m_event_tail = event.shared_from_this();
    outermost->m_event_tail = &event.m_next_completion;
    return true;
}

bool DriverWakeDeferral::DeferCompletion(WaitNode& node) noexcept
{
    auto* outermost = _TlsOutermost();
    if (outermost == nullptr)
        return false;

    *outermost->m_wait_node_tail = node.shared_from_this();
    outermost->m_wait_node_tail = &node.m_next_completion;
    return true;
}

DriverWakeDeferral::~DriverWakeDeferral() noexcept
{
    auto& outermost = _TlsOutermost();
    if (outermost != this)
        return;

    /**
     * 先脱离 TLS：完成回调或 Wake 重入时应建立自己的通知临界区。
     */
    outermost = nullptr;
    const auto pending = std::exchange(tl_pending, {});
    while (m_first_event != nullptr) {
        auto event = std::move(m_first_event);
        m_first_event = std::move(event->m_next_completion);
        event->_FinishTrigger(GetCoPollEventFlags(event->m_state.load(std::memory_order_relaxed)));
    }
    while (m_first_wait_node != nullptr) {
        auto node = std::move(m_first_wait_node);
        m_first_wait_node = std::move(node->m_next_completion);
        node->_Complete(GetCoPollEventFlags(node->m_state.load(std::memory_order_relaxed)));
    }
    if (pending.m_pending)
        Scheduler::_WakeProcesserDrivers(pending.m_target);
}

} // namespace bbt::coroutine::detail
