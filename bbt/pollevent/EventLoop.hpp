#pragma once
#include <bbt/pollevent/detail/EventBase.hpp>
#include <bbt/pollevent/detail/Define.hpp>
#include <memory>

namespace bbt::pollevent
{

class Event;

class EventLoop
{
    friend class Event;
public:
    typedef std::shared_ptr<EventLoop> SPtr;

    explicit EventLoop(detail::EventBase* base, bool need_auto_free_base = true);
    EventLoop();
    ~EventLoop();

    int                     StartLoop(int opt);
    int                     BreakLoop();

    int                     GetEventNum();

    std::shared_ptr<Event>  CreateEvent(evutil_socket_t fd, short events,
                                        const OnEventCallback& onevent_cb);

    int64_t                 GetEvMonotonic() const;
    int                     GetTimeOfDayCached(struct timeval *tv) const;
    int64_t                 GetTime() const;

    /* 内部 */
    detail::EventBase*      GetEventBase() { return m_ev_base; }

private:
    detail::EventBase*      m_ev_base{nullptr};
    bool                    m_auto_free_base{false};
};

} // namespace bbt::pollevent
