/**
 * @file WaitNode.hpp
 * @brief 纯 custom 协程等待的轻量一次性节点。
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/sync/CoEventValue.hpp>

namespace bbt::coroutine::detail {

class DriverWakeDeferral;

/**
 * @brief 不依赖 fd、timer 或 poller 的一次性等待节点。
 *
 * 节点实例就是一轮等待的 generation。owner 由创建者携带，完成回调只负责
 * 把首胜原因交回拥有该节点的 Coroutine。节点不拥有 Coroutine，避免等待
 * 完成与协程终态之间形成引用环。
 */
class WaitNode final : public std::enable_shared_from_this<WaitNode> {
public:
    using SPtr = std::shared_ptr<WaitNode>;
    using OnTrigger = std::function<void(short)>;

    static SPtr Create(CoroutineId owner, OnTrigger on_trigger);

    WaitNode(CoroutineId owner, OnTrigger on_trigger);
    ~WaitNode() = default;

    WaitNode(const WaitNode&) = delete;
    WaitNode& operator=(const WaitNode&) = delete;

    /**
     * 首胜触发。早到触发只锁存 PENDING，由 CommitPark 消费。
     */
    int Trigger(short trigger_events);

    /**
     * 挂起后登记。PENDING 允许登记成功，供早到路径继续提交。
     */
    int Regist();

    /**
     * Context 切回 Processer 后提交 park。
     * 返回 true 表示本线程消费了早到触发并执行了完成回调。
     */
    bool CommitPark();

    /**
     * 取消本轮节点，不产生完成回调。
     */
    int UnRegist();

    bool IsFinal() const noexcept;
    CoroutineId Owner() const noexcept { return m_owner; }
    uint64_t Generation() const noexcept { return m_generation; }

    void SetNotifyValue(const bbt::coroutine::CoEventValue& value) noexcept { m_notify_value = value; }
    bbt::coroutine::CoEventValue GetNotifyValue() const noexcept { return m_notify_value; }

private:
    friend class DriverWakeDeferral;

    enum class Phase : uint8_t {
        INITED,
        ARMING,
        ARMED,
        PARKED,
        PENDING,
        TRIGGERING,
        FINAL,
        CANCELLED,
    };

    static uint64_t _NextGeneration() noexcept;
    static uint64_t _Pack(Phase phase, short flags) noexcept;
    static Phase _Phase(uint64_t state) noexcept;
    static short _Flags(uint64_t state) noexcept;

    int _Complete(short trigger_events) noexcept;

    const CoroutineId m_owner;
    const uint64_t m_generation;
    const OnTrigger m_on_trigger;
    std::atomic<uint64_t> m_state;
    SPtr m_next_completion;
    bbt::coroutine::CoEventValue m_notify_value{};
};

}
