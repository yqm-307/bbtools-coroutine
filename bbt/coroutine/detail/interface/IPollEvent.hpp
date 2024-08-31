#pragma once

namespace bbt::coroutine::detail
{
class IPoller;

class IPollEvent
{
public:
    virtual int                 Trigger(short trigger_events, int customkey) = 0;
    virtual CoPollEventId       GetId() const = 0;
    virtual std::shared_ptr<Coroutine> GetWaitCo() const = 0;

    virtual int                 InitFdEvent(int fd, short event, int ms) = 0;
    virtual void                InitCustomEvent(CoPollEventCustom custom_key) = 0;
    virtual int                 Regist() = 0;
};

}