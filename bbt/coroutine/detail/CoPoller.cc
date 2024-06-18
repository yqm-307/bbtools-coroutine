#include <bbt/base/assert/Assert.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>

namespace bbt::coroutine::detail
{

CoPoller::UPtr& CoPoller::GetInstance()
{
    static UPtr _inst = nullptr;
    if (_inst == nullptr)
        _inst = UPtr{new CoPoller()};
    
    return _inst;
}


CoPoller::CoPoller():
    m_epoll_fd(::epoll_create(1024))
{
    AssertWithInfo(m_epoll_fd >= 0, "epoll_create() failed!");
}

CoPoller::~CoPoller()
{
    
}

int CoPoller::AddEvent(std::shared_ptr<IPollEvent> ievent, int addevent)
{
    int ret = 0;

    auto event = std::dynamic_pointer_cast<CoPollEvent>(ievent);
    auto& ev = event->GetEpollEvent();

    ev.events = addevent;
    auto ptr = new PrivData{event};
    ev.data.ptr = (void*)ptr;
    ret = epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, event->GetFd(), &ev);

    return ret;
}

int CoPoller::DelEvent(std::shared_ptr<IPollEvent> ievent, int delevent)
{
    int ret = 0;
    auto event = std::dynamic_pointer_cast<CoPollEvent>(ievent);
    auto& ev = event->GetEpollEvent();

    ret = ::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, event->GetFd(), NULL);
    ((PrivData*)ev.data.ptr)->event_sptr = nullptr;
    delete (PrivData*)ev.data.ptr;

    return ret;
}

int CoPoller::ModifyEvent(std::shared_ptr<IPollEvent> event, int modify_event)
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