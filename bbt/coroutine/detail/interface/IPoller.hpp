#pragma once
#include <memory>
#include <bbt/coroutine/detail/interface/IPollEvent.hpp>

namespace bbt::coroutine::detail
{

class IPoller
{
public:
    /**
     * @brief 注册一个事件
     * 
     * @param event 事件对象
     * @param addevent 关注的事件
     * @return 0表示成功，否则为-1
     */
    virtual int AddFdEvent(std::shared_ptr<IPollEvent> event, int fd) = 0;

    /**
     * @brief 删除一个事件
     * 
     * @param event 事件对象
     * @param delevent 删除的事件
     * @return 0表示成功，否则为-1
     */
    virtual int DelFdEvent(int fd) = 0;

    /**
     * @brief 修改监听中的事件
     * 
     * @param event 事件对象
     * @param target_event 修改后的事件
     * @return 0表示成功，否则为-1
     */
    virtual int ModifyFdEvent(std::shared_ptr<IPollEvent> event, int fd, int event_opt) = 0;

    /**
     * @brief 执行一次轮询，触发所有完成的事件
     * 
     * @return 触发的事件数量
     */
    virtual int PollOnce() = 0;

};

}