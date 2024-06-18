#include <stdio.h>
#include <fcntl.h>
#include <bbt/base/assert/Assert.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>

namespace bbt::coroutine::detail
{

CoPollEvent::SPtr CoPollEvent::Create(std::shared_ptr<Coroutine> coroutine, int timeout, const CoPollEventCallback& cb)
{
    return std::make_shared<CoPollEvent>(coroutine, PollEventType::POLL_EVENT_TIMEOUT, timeout, cb);
}


CoPollEvent::CoPollEvent(std::shared_ptr<Coroutine> coroutine, PollEventType type, int timeout, const CoPollEventCallback& cb):
    m_coroutine(coroutine),
    m_type(type),
    m_timeout(timeout),
    m_fd(::timerfd_create(CLOCK_REALTIME, 0)),
    m_onevent_callback(cb)
{
    Assert(m_coroutine != nullptr);
    Assert(m_timeout > 0);
    Assert(m_fd >= 0);
    Assert(m_onevent_callback != nullptr);
    m_events = 0;

}


CoPollEvent::~CoPollEvent()
{
    ::close(m_fd);
}

int CoPollEvent::GetFd()
{
    return m_fd;
}

void CoPollEvent::Trigger(IPoller* poller, int trigger_events)
{
    /**
     * 触发事件实际操作由创建者定义，实现完全解耦。
     * 
     * 对于CoPoller、CoPollEvent来说，都不需要关心事件完成后
     * 到底执行什么样的操作了，只由外部创建者定义。
     */
    if (m_onevent_callback != nullptr)
        m_onevent_callback(m_coroutine);

    /* 因为使用了 EPOLLONESHOT 所以触发超时任务后，主动释放资源 */
    ((CoPoller::PrivData*)m_epoll_event.data.ptr)->event_sptr = nullptr;
    delete (CoPoller::PrivData*)m_epoll_event.data.ptr;
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

    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;
    

    int64_t nsec = now.tv_nsec + (m_timeout % 1000) * 1000000;

    new_value.it_value.tv_sec = now.tv_sec + (m_timeout / 1000) + (nsec >= 1000000000 ? 1 : 0);
    new_value.it_value.tv_nsec = (nsec >= 1000000000 ? nsec - 1000000000 : nsec);
    new_value.it_interval.tv_nsec = 0;
    new_value.it_interval.tv_sec = 0;

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