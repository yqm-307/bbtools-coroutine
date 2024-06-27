#include <stdio.h>
#include <fcntl.h>
#include <bbt/base/assert/Assert.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>

namespace bbt::coroutine::detail
{

CoPollEvent::SPtr CoPollEvent::Create(std::shared_ptr<Coroutine> coroutine, const CoPollEventCallback& cb)
{
    return std::make_shared<CoPollEvent>(coroutine, cb);
}

CoPollEvent::CoPollEvent(std::shared_ptr<Coroutine> coroutine, const CoPollEventCallback& cb):
    m_coroutine(coroutine),
    m_onevent_callback(cb)
{
    Assert(m_coroutine != nullptr);
    Assert(m_onevent_callback != nullptr);
    m_run_status = CoPollEventStatus::POLLEVENT_INITED;
}

CoPollEvent::~CoPollEvent()
{
    if (m_fd >= 0) ::close(m_fd);
    if (m_timerfd >= 0) ::close(m_timerfd);
}

int CoPollEvent::GetFd() const
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
    /* 取消所有系统fd事件并释放资源 */
    Assert(_CannelAllFdEvent() == 0);

    if (m_run_status != CoPollEventStatus::POLLEVENT_LISTEN)
        return;

    m_run_status = CoPollEventStatus::POLLEVENT_TRIGGER;

    if (m_onevent_callback != nullptr) {
        m_onevent_callback(shared_from_this(), m_coroutine);
    }

    _OnFinal();
}

void CoPollEvent::_OnFinal()
{
    m_run_status = CoPollEventStatus::POLLEVENT_FINAL;
    /**
     * 自定义事件，不会再次触发了
     * 系统相关的fd事件，需要手动释放掉
     */

    _CannelAllFdEvent();

}


int CoPollEvent::GetEvent() const
{
    return m_type;
}

int CoPollEvent::InitFdReadableEvent(int fd)
{
    int ret = 0;
    if (m_run_status >= CoPollEventStatus::POLLEVENT_LISTEN || m_fd >= 0)
        return -1;

    m_fd = fd;
    return ret;
}

int CoPollEvent::InitTimeoutEvent(int timeout)
{
    int ret = 0;
    if (m_run_status >= CoPollEventStatus::POLLEVENT_LISTEN || timeout <= 0 || m_timerfd >= 0)
        return -1;

    m_timeout = timeout;
    m_timerfd = ::timerfd_create(CLOCK_REALTIME, 0);
    if (m_timerfd < 0)
        return -1;
    
    itimerspec new_value;
    timespec   now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;
    
    int64_t nsec = now.tv_nsec + (m_timeout % 1000) * 1000000;

    new_value.it_value.tv_sec = now.tv_sec + (m_timeout / 1000) + (nsec >= 1000000000 ? 1 : 0);
    new_value.it_value.tv_nsec = (nsec >= 1000000000 ? nsec - 1000000000 : nsec);
    new_value.it_interval.tv_nsec = 0;
    new_value.it_interval.tv_sec = 0;

    if (timerfd_settime(m_fd, TFD_TIMER_ABSTIME, &new_value, NULL) != 0)
        return -1;
    
    _CreateEpollEvent(EPOLLIN | EPOLLONESHOT);

    ret = g_bbt_poller->AddFdEvent(shared_from_this(), m_timerfd);
    if (ret != 0) {
        std::unique_lock<std::mutex> _(m_epoll_event_map_mutex);
        _DestoryEpollEvent(m_timerfd);
    }

    return ret;
}

int CoPollEvent::InitCustomEvent(int key, void* args)
{
    if (m_run_status >= CoPollEventStatus::POLLEVENT_LISTEN || m_has_custom_event)
        return -1;

    m_has_custom_event = true;
    return 0;
}

int CoPollEvent::Regist()
{
    int ret = 0;
    m_run_status = CoPollEventStatus::POLLEVENT_LISTEN;

    /* fd 事件 */
    if (m_fd >= 0)
        ret = _RegistFdEvent();

    /* 超时事件 */
    if (m_timerfd >= 0)
        ret = _RegistTimeoutEvent();

    /* 自定义事件 */
    if (m_has_custom_event)
        ret = _RegistCustomEvent();

    if (ret != 0)
        return ret;

    _OnListen();
    return ret;
}

int CoPollEvent::_CreateEpollEvent(int fd, int events)
{
    epoll_event event;
    event.events = events;
    event.data.ptr = new PrivData{shared_from_this()};

    std::unique_lock<std::mutex> _(m_epoll_event_map_mutex);
    auto it = m_epoll_event_map.find(fd);
    if (it != m_epoll_event_map.end())
        return -1;
    
    m_epoll_event_map[fd] = event;
    return 0;
}

void CoPollEvent::_DestoryEpollEvent(int fd)
{
    // std::unique_lock<std::mutex> _(m_epoll_event_map_mutex);
    auto it = m_epoll_event_map.find(fd);
    if (it == m_epoll_event_map.end())
        return;
    auto event = it->second;

    if (event.data.ptr == nullptr)
        return;

    ((PrivData*)event.data.ptr)->event_sptr = nullptr;
    delete (PrivData*)event.data.ptr;
}

void CoPollEvent::_OnListen()
{
    int ret = 0;
    m_run_status = CoPollEventStatus::POLLEVENT_LISTEN;
}

int CoPollEvent::UnRegistEvent()
{
    int ret = 0;

    /* 只能对监听中的任务执行操作 */
    if (m_run_status != CoPollEventStatus::POLLEVENT_LISTEN)
        return -1;

    if (ret == 0)
    {
        _CannelAllFdEvent();
        m_run_status = CoPollEventStatus::POLLEVENT_CANNEL;
    }

    return ret;
}

int CoPollEvent::_RegistCustomEvent()
{
    return 0;
}

int CoPollEvent::_RegistTimeoutEvent()
{
    int ret = 0;
    itimerspec new_value;
    timespec   now;

    if (m_timerfd < 0)
        return -1;
    
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;
    
    int64_t nsec = now.tv_nsec + (m_timeout % 1000) * 1000000;

    new_value.it_value.tv_sec = now.tv_sec + (m_timeout / 1000) + (nsec >= 1000000000 ? 1 : 0);
    new_value.it_value.tv_nsec = (nsec >= 1000000000 ? nsec - 1000000000 : nsec);
    new_value.it_interval.tv_nsec = 0;
    new_value.it_interval.tv_sec = 0;

    if (timerfd_settime(m_timerfd, TFD_TIMER_ABSTIME, &new_value, NULL) != 0)
        return -1;
    
    _CreateEpollEvent(m_timerfd, EPOLLIN | EPOLLONESHOT);

    ret = g_bbt_poller->AddFdEvent(shared_from_this(), m_timerfd);
    if (ret != 0) {
        std::unique_lock<std::mutex> _(m_epoll_event_map_mutex);
        _DestoryEpollEvent(m_timerfd);
    }

    return ret;
}

int CoPollEvent::_RegistFdEvent()
{
    int ret = 0;
    if (m_fd < 0)
        return -1;

    ret = _CreateEpollEvent(m_fd, EPOLLIN | EPOLLONESHOT | EPOLLET);
    if (ret != 0)
        return ret;

    ret = g_bbt_poller->AddFdEvent(shared_from_this(), m_fd);

    if (ret != 0) {
        std::unique_lock<std::mutex> _(m_epoll_event_map_mutex);
        _DestoryEpollEvent(m_fd);
    }

    return ret;
}

int CoPollEvent::_CannelAllFdEvent()
{
    int ret = 0;
    if (m_run_status != CoPollEventStatus::POLLEVENT_LISTEN)
        return -1;
    
    std::unique_lock<std::mutex> _(m_epoll_event_map_mutex);

    for (auto&& it : m_epoll_event_map) {
        Assert(g_bbt_poller->DelFdEvent(it.first) == 0);
        _DestoryEpollEvent(it.first);
    }

    return ret;
}

epoll_event* CoPollEvent::GetEpollEvent(int fd)
{
    std::unique_lock<std::mutex> _(m_epoll_event_map_mutex);
    auto it = m_epoll_event_map.find(fd);
    if (it == m_epoll_event_map.end())
        return nullptr;

    return &(it->second);
}

bool CoPollEvent::IsListening() const
{
    return (m_run_status == CoPollEventStatus::POLLEVENT_LISTEN);
}

bool CoPollEvent::IsFinal() const
{
    return (m_run_status == CoPollEventStatus::POLLEVENT_FINAL);
}

CoPollEventStatus CoPollEvent::GetStatus() const
{
    return m_run_status;
}


}