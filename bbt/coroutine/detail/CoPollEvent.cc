#include <stdio.h>
#include <fcntl.h>
#include <bbt/base/assert/Assert.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>

namespace bbt::coroutine::detail
{

CoPollEvent::SPtr CoPollEvent::Create(std::shared_ptr<Coroutine> coroutine, int timeout)
{
    return std::make_shared<CoPollEvent>(coroutine, PollEventType::POLL_EVENT_TIMEOUT, timeout);
}


CoPollEvent::CoPollEvent(std::shared_ptr<Coroutine> coroutine, PollEventType type, int timeout):
    m_coroutine(coroutine),
    m_type(type),
    m_timeout(timeout),
    m_fd(::timerfd_create(CLOCK_REALTIME, O_NONBLOCK))
{
    Assert(m_coroutine != nullptr);
    Assert(m_timeout > 0);
    Assert(m_fd >= 0);
    m_events = 0;

}


CoPollEvent::~CoPollEvent()
{
}

int CoPollEvent::GetFd()
{
    return m_fd;
}

void CoPollEvent::Trigger(IPoller* poller, int trigger_events)
{
    printf("[%ld]trigger event! event=%d\n", bbt::clock::now<>().time_since_epoch().count(), trigger_events);
    Assert(CoPoller::GetInstance()->DelEvent(shared_from_this(), EPOLL_EVENTS::EPOLLIN) == 0);
}

int CoPollEvent::GetEvent()
{
    return m_events;
}

int CoPollEvent::_RegistTimeoutEvent()
{
    // 计算超时时间
    itimerspec new_value; 
    timespec now;
    memset(&new_value, '\0', sizeof(new_value));

    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;

    new_value.it_value.tv_sec = now.tv_sec + (m_timeout / 1000);
    new_value.it_value.tv_nsec = now.tv_nsec + (m_timeout % 1000) * 1000;

    if (timerfd_settime(m_fd, TFD_TIMER_ABSTIME, &new_value, NULL) != 0)
        return -1;

    if (CoPoller::GetInstance()->AddEvent(shared_from_this(), EPOLL_EVENTS::EPOLLIN | EPOLL_EVENTS::EPOLLONESHOT) != 0)
        return -1;
    
    return 0;
}

int CoPollEvent::RegistEvent()
{
    switch (m_type)
    {
    case PollEventType::POLL_EVENT_TIMEOUT :
        return _RegistTimeoutEvent();
    
    default:
        AssertWithInfo(false, "还没有开始做");
        break;
    }
    return -1;
}

epoll_event& CoPollEvent::GetEpollEvent()
{
    return m_epoll_event;
}


}