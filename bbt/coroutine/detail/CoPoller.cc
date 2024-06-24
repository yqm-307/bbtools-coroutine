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
    m_epoll_fd(::epoll_create(65535))
{
    AssertWithInfo(m_epoll_fd >= 0, "epoll_create() failed!");
}

CoPoller::~CoPoller()
{
    
}

int CoPoller::AddEvent(std::shared_ptr<IPollEvent> ievent)
{
    auto co_event = std::dynamic_pointer_cast<CoPollEvent>(ievent);

    /* 防止重复注册 */
    if (co_event->GetStatus() != CoPollEventStatus::POLLEVENT_INITED)
        return -1;

    auto& ev = co_event->GetEpollEvent();

    return epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, co_event->GetFd(), &ev);
}

int CoPoller::DelEvent(std::shared_ptr<IPollEvent> ievent)
{
    int ret = 0;
    auto co_event = std::dynamic_pointer_cast<CoPollEvent>(ievent);

    return ::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, co_event->GetFd(), NULL);
}

int CoPoller::ModifyEvent(std::shared_ptr<IPollEvent> event)
{
    // int ret = 0;
    // epoll_event ev;

    // ev.events = modify_event;
    // ret = ::epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, event->GetFd(), &ev);

    return -1;
}

int CoPoller::PollOnce()
{
    const int max_event_num = 1024;
    epoll_event events[max_event_num];
    /* 获取系统关注事件中，被触发的事件 */
    int active_event_num = ::epoll_wait(m_epoll_fd, events, max_event_num, 0);

    for (int i = 0; i < active_event_num; ++i)
    {
        auto& event = events[i];
        auto privdata = reinterpret_cast<CoPollEvent::PrivData*>(event.data.ptr);
        auto event_sptr = std::dynamic_pointer_cast<CoPollEvent>(privdata->event_sptr);

        /* 注销事件，并触发事件通知监听者 */
        event_sptr->_CannelRegistEvent();
        event_sptr->Trigger(this, event.events);
    }

    return active_event_num;
}

}