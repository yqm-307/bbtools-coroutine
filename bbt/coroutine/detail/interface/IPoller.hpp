#pragma once
#include <memory>
#include <bbt/coroutine/detail/interface/IPollEvent.hpp>

/**
 * @file IPoller.hpp
 * @brief 底层就绪事件注册、删除、修改与等待的遗留抽象。
 */

namespace bbt::coroutine::detail {

/** @brief 可替换 backend 的最小接口；遗留接口，CoPoller 未实现，EventLoop 契约见 CoPoller.hpp。 */
class IPoller {
public:
    /** @brief 注册 @p fd 及其事件对象（就绪时回调其 Trigger）；@return 0 成功，否则 -1。 */
    virtual int AddFdEvent(std::shared_ptr<IPollEvent> event, int fd) = 0;

    /** @brief 删除 @p fd 上的事件；@return 0 成功，否则 -1。 */
    virtual int DelFdEvent(int fd) = 0;

    /** @brief 修改 @p fd 关注的事件位 @p event_opt；@return 0 成功，否则 -1。 */
    virtual int ModifyFdEvent(std::shared_ptr<IPollEvent> event, int fd, int event_opt) = 0;

    /** @brief 执行一次轮询，触发本轮所有就绪事件；@return 本轮触发数量。 */
    virtual int PollOnce() = 0;
};

}