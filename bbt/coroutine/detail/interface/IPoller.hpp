#pragma once
#include <memory>
#include <bbt/coroutine/detail/interface/IPollEvent.hpp>

namespace bbt::coroutine::detail
{

class IPoller
{
public:
    /**
     * @brief 注册一个等待事件
     * 
     * 如果fd大于等于0，则poller监听event事件，事件完成时加入就绪队列
     * 如果fd小于等于0，则poller相当于根据timeout_ms的定时任务，超时加入就绪队列
     * 
     * @return int 0表示成功，否则为-1
     */
    virtual int AddEvent(std::shared_ptr<IPollEvent> event, int addevent) = 0;

    /**
     * @brief 删除event上等待的事件，如果监听的事件全部删除，取消监听
     * 
     * @param event         操作的事件对象 
     * @param delevent      删除的事件类型
     * @return int 0表示成功，否则为-1
     */
    virtual int DelEvent(std::shared_ptr<IPollEvent> event, int delevent) = 0;

    /**
     * @brief 将监听中的event事件类型修改为target_event
     * 
     * @param event 
     * @param target_event 
     * @return int 
     */
    virtual int ModifyEvent(std::shared_ptr<IPollEvent> event, int opt, int modify_event) = 0;

    /**
     * @brief 轮询一次所有事件
     */
    virtual void PollOnce() = 0;

};

}