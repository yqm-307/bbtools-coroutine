#include <bbt/base/assert/Assert.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>

namespace bbt::coroutine::detail
{


CoPoller::CoPoller():
    m_epoll_fd(epoll_create(0))
{
    AssertWithInfo(m_epoll_fd >= 0, "epoll_create() failed!");
}

CoPoller::~CoPoller()
{
    
}

int CoPoller::AddEvent(std::shared_ptr<IPollEvent> event, int addevent)
{
    int ret = 0;
    epoll_event ev;

    ev.events = addevent;
    ev.data.ptr = new PrivData{event};
    ret = epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, event->GetFd(), &ev);

    return ret;
}

int CoPoller::DelEvent(std::shared_ptr<IPollEvent> event, int delevent)
{
    int ret = 0;
    epoll_event ev;

    ret = ::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, event->GetFd(), &ev);
    delete ev.data.ptr;

    return ret;
}

int CoPoller::ModifyEvent(std::shared_ptr<IPollEvent> event, int opt, int modify_event)
{
    int ret = 0;
    epoll_event ev;

    ev.events = modify_event;
    ret = ::epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, event->GetFd(), &ev);

    return ret;
}

void CoPoller::PollOnce()
{
    const int max_event_num = 1024;
    epoll_event events[max_event_num];
    int active_event_num = ::epoll_wait(m_epoll_fd, events, max_event_num, 20);
    for (int i = 0; i < active_event_num; ++i)
    {
        auto& event = events[i];
        auto privdata = reinterpret_cast<PrivData*>(event.data.ptr);
        auto event_sptr = std::dynamic_pointer_cast<CoPollEvent>(privdata->event_sptr);
        event_sptr->Trigger(this, event.events);
    }
}

}