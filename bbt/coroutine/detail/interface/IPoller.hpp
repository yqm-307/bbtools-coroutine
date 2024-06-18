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
    virtual int AddEvent(std::shared_ptr<IPollEvent> event, int addevent) = 0;

    /**
     * @brief 删除一个事件
     * 
     * @param event 事件对象
     * @param delevent 删除的事件
     * @return 0表示成功，否则为-1
     */
    virtual int DelEvent(std::shared_ptr<IPollEvent> event, int delevent) = 0;

    /**
     * @brief 修改监听中的事件
     * 
     * @param event 事件对象
     * @param target_event 修改后的事件
     * @return 0表示成功，否则为-1
     */
    virtual int ModifyEvent(std::shared_ptr<IPollEvent> event, int modify_event) = 0;

    /**
     * @brief 执行一次轮询，触发所有完成的事件
     * 
     */
    virtual void PollOnce() = 0;

};

}