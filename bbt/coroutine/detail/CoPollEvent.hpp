#pragma once
#include <bbt/coroutine/detail/interface/IPollEvent.hpp>

namespace bbt::coroutine::detail
{

class CoPollEvent:
    public IPollEvent
{
public:
    CoPollEvent();
    ~CoPollEvent();

    virtual int                     GetFd() override;
    virtual void                    Trigger(IPoller* poller, int trigger_events) override;
private:
    int m_fd;
};

}