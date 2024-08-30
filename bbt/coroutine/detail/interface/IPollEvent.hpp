#pragma once

namespace bbt::coroutine::detail
{
class IPoller;

class IPollEvent
{
public:
    virtual int Trigger(short trigger_events, int customkey) = 0;
    virtual CoPollEventId GetId() const = 0;
};

}