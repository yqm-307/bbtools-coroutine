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

CoPollEvent::SPtr CoPollEvent::Create(std::shared_ptr<Coroutine> coroutine, int fd, int timeout, const CoPollEventCallback& cb)
{
    return std::make_shared<CoPollEvent>(coroutine, PollEventType::POLL_EVENT_READABLE, fd, timeout, cb);
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
    memset(&m_epoll_event, '\0', sizeof(m_epoll_event));
    m_run_status = CoPollEventStatus::POLLEVENT_INITED;
}

CoPollEvent::CoPollEvent(std::shared_ptr<Coroutine> coroutine, PollEventType type, int fd, int timeout, const CoPollEventCallback& cb):
    m_coroutine(coroutine),
    m_type(type),
    m_timeout(timeout),
    m_fd(fd),
    m_onevent_callback(cb)
{
    Assert(m_coroutine != nullptr);
    Assert(m_timeout > 0);
    Assert(m_fd >= 0);
    Assert(m_onevent_callback != nullptr);
    memset(&m_epoll_event, '\0', sizeof(m_epoll_event));
    m_run_status = CoPollEventStatus::POLLEVENT_INITED;
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
    _DestoryEpollEvent();

    /* 标记事件已经完成 */
    m_run_status = CoPollEventStatus::POLLEVENT_FINAL;
}

int CoPollEvent::GetEvent()
{
    return m_epoll_event.events;
}

int CoPollEvent::_RegistTimeoutEvent()
{
    // 计算超时时间
    itimerspec new_value; 
    timespec now;
    int ret = 0;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;
    

    int64_t nsec = now.tv_nsec + (m_timeout % 1000) * 1000000;

    new_value.it_value.tv_sec = now.tv_sec + (m_timeout / 1000) + (nsec >= 1000000000 ? 1 : 0);
    new_value.it_value.tv_nsec = (nsec >= 1000000000 ? nsec - 1000000000 : nsec);
    new_value.it_interval.tv_nsec = 0;
    new_value.it_interval.tv_sec = 0;

    if (timerfd_settime(m_fd, TFD_TIMER_ABSTIME, &new_value, NULL) != 0)
        return -1;

    _CreateEpollEvent(EPOLL_EVENTS::EPOLLIN | EPOLL_EVENTS::EPOLLONESHOT);

    ret = CoPoller::GetInstance()->AddEvent(shared_from_this());

    /* 注册失败，销毁epoll_event对象 */
    if (ret != 0) _DestoryEpollEvent();
    
    return ret;
}

int CoPollEvent::_RegistReadableEvent()
{
    int ret = 0;

    _CreateEpollEvent(EPOLL_EVENTS::EPOLLIN | EPOLL_EVENTS::EPOLLONESHOT);

    ret = CoPoller::GetInstance()->AddEvent(shared_from_this());

    /* 注册失败，销毁epoll_event对象 */
    if (ret != 0) _DestoryEpollEvent();

    return -1;
}

void CoPollEvent::_CreateEpollEvent(int events)
{
    m_epoll_event.events = events;
    m_epoll_event.data.ptr = new PrivData{shared_from_this()};
}

void CoPollEvent::_DestoryEpollEvent()
{
    ((PrivData*)m_epoll_event.data.ptr)->event_sptr = nullptr;
    delete (PrivData*)m_epoll_event.data.ptr;
}

int CoPollEvent::RegistEvent()
{
    int ret = 0;
    switch (m_type)
    {
    case PollEventType::POLL_EVENT_TIMEOUT:
        ret = _RegistTimeoutEvent();
        break;
    case PollEventType::POLL_EVENT_READABLE:
        ret = _RegistReadableEvent();
        break;
    default:
        AssertWithInfo(false, "还没有开始做");
        break;
    }

    m_run_status = (ret == 0 ? CoPollEventStatus::POLLEVENT_LISTEN : m_run_status);
    return ret;
}

int CoPollEvent::UnRegistEvent()
{
    int ret = 0;

    /* 只能对监听中的任务执行操作 */
    if (m_run_status != CoPollEventStatus::POLLEVENT_LISTEN)
        return -1;

    ret = CoPoller::GetInstance()->DelEvent(shared_from_this());
    
    if (ret == 0)
    {
        _DestoryEpollEvent();
        m_run_status = CoPollEventStatus::POLLEVENT_FINAL;
    }

    return ret;
}


epoll_event& CoPollEvent::GetEpollEvent()
{
    return m_epoll_event;
}

bool CoPollEvent::IsListening()
{
    return (m_run_status == CoPollEventStatus::POLLEVENT_LISTEN);
}

bool CoPollEvent::IsFinal()
{
    return (m_run_status == CoPollEventStatus::POLLEVENT_FINAL);
}

CoPollEventStatus CoPollEvent::GetStatus()
{
    return m_run_status;
}


}