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

int CoPoller::AddFdEvent(std::shared_ptr<IPollEvent> ievent, int fd)
{
    auto co_event = std::dynamic_pointer_cast<CoPollEvent>(ievent);

    /* 防止重复注册 */
    if (co_event->GetStatus() != CoPollEventStatus::POLLEVENT_INITED)
        return -1;

    auto ev = co_event->GetEpollEvent(fd);

    return epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, ev);
}

int CoPoller::DelFdEvent(int fd)
{
    int ret = 0;
    return ::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

int CoPoller::ModifyFdEvent(std::shared_ptr<IPollEvent> event, int fd, int event_opt)
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
    int active_event_num = ::epoll_wait(m_epoll_fd, events, max_event_num, 0);

    /* 通知触发的内核fd事件 */
    for (int i = 0; i < active_event_num; ++i)
    {
        auto& event = events[i];
        auto privdata = reinterpret_cast<CoPollEvent::PrivData*>(event.data.ptr);
        auto event_sptr = std::dynamic_pointer_cast<CoPollEvent>(privdata->event_sptr);

        /* 注销事件，并触发事件通知监听者 */
        // event_sptr->_CannelAllFdEvent();
        event_sptr->Trigger(this, event.events);
    }

    std::queue<std::shared_ptr<IPollEvent>> m_swap_queue;
    {
        std::unique_lock<std::mutex> _(m_custom_event_active_queue_mutex);
        m_swap_queue.swap(m_custom_event_active_queue);
    }

    active_event_num += m_swap_queue.size();

    /* 通知触发的自定义事件 */
    while (!m_swap_queue.empty())
    {
        auto item = m_swap_queue.front();
        item->Trigger(this, POLL_EVENT_CUSTOM);
        m_swap_queue.pop();
        std::unique_lock<std::mutex> _(m_custom_event_active_queue_mutex);
        Assert(m_safe_active_set.erase(item) > 0);
    }

    return active_event_num;
}

int CoPoller::NotifyCustomEvent(std::shared_ptr<IPollEvent> event)
{
    std::unique_lock<std::mutex> _(m_custom_event_active_queue_mutex);
    // AssertWithInfo(m_safe_active_set.find(event) != m_safe_active_set.end(), "有bug，这里不应该有重复的事件");
    if (m_safe_active_set.find(event) != m_safe_active_set.end())
        return -1;

    m_safe_active_set.insert(event);
    m_custom_event_active_queue.push(event);

    return 0;
}

}