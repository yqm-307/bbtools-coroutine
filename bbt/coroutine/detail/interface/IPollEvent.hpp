#pragma once

/**
 * @file IPollEvent.hpp
 * @brief 轮询事件的遗留抽象：由 IPoller 在就绪时回调 Trigger。
 */

namespace bbt::coroutine::detail {
class IPoller;

/** @brief 轮询事件接口；未被 CoPollEvent 实现，也不参与当前 EventLoop 路径（见 CoPoller.hpp），换 backend 改 CoPoller。 */
class IPollEvent {
public:
    /** @brief 就绪回调：@p poller 触发它，@p trigger_events 是就绪的事件位。 */
    virtual void Trigger(IPoller* poller, int trigger_events) = 0;

    /** @brief 本事件关注/已就绪的事件位。 */
    virtual int GetEvent() const = 0;
};

}