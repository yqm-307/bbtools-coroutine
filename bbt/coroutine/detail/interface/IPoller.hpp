#pragma once
#include <memory>
#include <bbt/coroutine/detail/interface/IPollEvent.hpp>

namespace bbt::coroutine::detail
{

class IPoller
{
public:

    virtual int Regist(std::shared_ptr<IPollEvent> event) = 0;
    virtual int UnRegist(CoPollEventId id) = 0;
    virtual int OnTrigger(CoPollEventId id) = 0;

    /**
     * @brief 执行一次轮询，触发所有完成的事件
     * 
     * @return 触发的事件数量
     */
    virtual bool PollOnce() = 0;

};

}