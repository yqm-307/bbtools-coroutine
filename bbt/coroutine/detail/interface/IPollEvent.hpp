#pragma once

namespace bbt::coroutine::detail
{
class IPoller;

class IPollEvent
{
public:
    virtual int Trigger(short trigger_events) = 0;
    virtual CoPollEventId GetId() = 0;
};

}