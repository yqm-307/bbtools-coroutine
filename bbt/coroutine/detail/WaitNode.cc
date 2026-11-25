#include <cstdio>
#include <exception>
#include <bbt/coroutine/detail/WaitNode.hpp>
#include <bbt/coroutine/detail/DriverWakeDeferral.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

namespace bbt::coroutine::detail {

namespace {

std::atomic<uint64_t> g_next_wait_generation{0};

void ReportWaitNodeException(CoroutineId owner, uint64_t generation, const char* message) noexcept
{
    auto& config = *g_bbt_coroutine_config;
    try {
        const auto& callback = config.RuntimeSnapshot().coevent_exception_callback;
        if (callback != nullptr) {
            callback(core::errcode::Errcode(message));
            return;
        }
    } catch (...) {
        /**
         * 异常处理器也可能失败，退回到不分配的日志与计数。
         */
    }
    std::fprintf(
        stderr,
        "[bbtco] unhandled wait node exception co=%llu generation=%llu: %.200s\n",
        static_cast<unsigned long long>(owner),
        static_cast<unsigned long long>(generation),
        message);
    config.m_unhandled_exception_count.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

WaitNode::SPtr WaitNode::Create(CoroutineId owner, OnTrigger on_trigger, OnInterrupt on_interrupt)
{
    return std::make_shared<WaitNode>(owner, std::move(on_trigger), std::move(on_interrupt));
}

WaitNode::WaitNode(CoroutineId owner, OnTrigger on_trigger, OnInterrupt on_interrupt)
    : m_owner(owner)
    , m_generation(_NextGeneration())
    , m_on_trigger(std::move(on_trigger))
    , m_on_interrupt(std::move(on_interrupt))
    , m_state(_Pack(Phase::INITED, 0))
{
}

uint64_t WaitNode::_NextGeneration() noexcept
{
    return g_next_wait_generation.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint64_t WaitNode::_Pack(Phase phase, short flags) noexcept
{
    return static_cast<uint64_t>(phase) | (static_cast<uint64_t>(static_cast<uint16_t>(flags)) << 8U);
}

WaitNode::Phase WaitNode::_Phase(uint64_t state) noexcept
{
    return static_cast<Phase>(state & 0xffU);
}

short WaitNode::_Flags(uint64_t state) noexcept
{
    return static_cast<short>((state >> 8U) & 0xffffU);
}

int WaitNode::Trigger(short trigger_events)
{
    if (trigger_events == 0)
        return -1;

    auto state = m_state.load(std::memory_order_acquire);
    for (;;) {
        const auto phase = _Phase(state);
        if (phase == Phase::INITED || phase == Phase::ARMED) {
            if (m_state.compare_exchange_weak(
                    state, _Pack(Phase::PENDING, trigger_events), std::memory_order_acq_rel, std::memory_order_acquire))
                return 0;
            continue;
        }

        if (phase != Phase::PARKED)
            return -1;

        if (m_state.compare_exchange_weak(
                state,
                _Pack(Phase::TRIGGERING, trigger_events),
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (DriverWakeDeferral::DeferCompletion(*this))
                return 0;
            return _Complete(trigger_events);
        }
    }
}

void WaitNode::Interrupt(short events)
{
    if (!m_on_interrupt || m_on_interrupt(events))
        Trigger(events);
}

int WaitNode::Regist()
{
    /**
     * 纯等待不向 poller 登记，中间没有需要保留的半登记状态。
     */
    uint64_t expected = _Pack(Phase::INITED, 0);
    if (m_state.compare_exchange_strong(
            expected, _Pack(Phase::ARMED, 0), std::memory_order_acq_rel, std::memory_order_acquire)) {
        return 0;
    }

    return _Phase(expected) == Phase::PENDING ? 0 : -1;
}

bool WaitNode::CommitPark()
{
    auto state = m_state.load(std::memory_order_acquire);
    for (;;) {
        const auto phase = _Phase(state);
        if (phase == Phase::ARMED) {
            if (m_state.compare_exchange_weak(
                    state, _Pack(Phase::PARKED, 0), std::memory_order_acq_rel, std::memory_order_acquire))
                return false;
            continue;
        }

        if (phase != Phase::PENDING)
            return false;

        const auto trigger_events = _Flags(state);
        if (m_state.compare_exchange_weak(
                state,
                _Pack(Phase::TRIGGERING, trigger_events),
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            _Complete(trigger_events);
            return true;
        }
    }
}

int WaitNode::UnRegist()
{
    auto state = m_state.load(std::memory_order_acquire);
    for (;;) {
        const auto phase = _Phase(state);
        if (phase != Phase::INITED && phase != Phase::ARMED && phase != Phase::PARKED)
            return -1;

        if (m_state.compare_exchange_weak(
                state, _Pack(Phase::CANCELLED, 0), std::memory_order_acq_rel, std::memory_order_acquire))
            return 0;
    }
}

bool WaitNode::IsFinal() const noexcept
{
    const auto phase = _Phase(m_state.load(std::memory_order_acquire));
    return phase == Phase::FINAL || phase == Phase::CANCELLED;
}

int WaitNode::_Complete(short trigger_events) noexcept
{
    m_state.store(_Pack(Phase::FINAL, trigger_events), std::memory_order_release);
    /**
     * 回调可能已经发布恢复任务；失败只报告，不能重试首胜完成。
     */
    try {
        if (m_on_trigger)
            m_on_trigger(trigger_events);
    } catch (const std::exception& e) {
        ReportWaitNodeException(m_owner, m_generation, e.what());
    } catch (...) {
        ReportWaitNodeException(m_owner, m_generation, "unknown exception");
    }
    return 0;
}

} // namespace bbt::coroutine::detail
