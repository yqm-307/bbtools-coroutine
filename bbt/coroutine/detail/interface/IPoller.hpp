#pragma once
#include <memory>
#include <bbt/coroutine/detail/interface/IPollEvent.hpp>

namespace bbt::coroutine::detail
{

class IPoller
{
public:

    virtual std::pair<int, CoPollEventId> Regist(std::shared_ptr<IPollEvent> event) = 0;
    virtual int UnRegist(CoPollEventId id) = 0;
    virtual int Notify(CoPollEventId id, short trigger_event, CoPollEventCustom custom_key) = 0;

    /**
     * @brief 执行一次轮询，触发所有完成的事件
     * 
     * @return 触发的事件数量
     */
    virtual bool PollOnce() = 0;

};

}