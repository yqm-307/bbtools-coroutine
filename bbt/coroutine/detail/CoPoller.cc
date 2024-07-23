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


CoPoller::CoPoller()
{
    auto* base = new bbt::pollevent::EventBase(
        bbt::pollevent::EventBaseConfigFlag::NO_CACHE_TIME |
        bbt::pollevent::EventBaseConfigFlag::PRECISE_TIMER);
    
    Assert(base != nullptr);

    m_event_loop = std::make_shared<bbt::pollevent::EventLoop>(base, true);
    Assert(m_event_loop != nullptr);
}

CoPoller::~CoPoller()
{
}


std::shared_ptr<bbt::pollevent::Event> CoPoller::CreateEvent(int fd, short events, const bbt::pollevent::OnEventCallback& onevent_cb)
{
    return m_event_loop->CreateEvent(fd, events, onevent_cb);
}

bool CoPoller::PollOnce()
{
    errno = 0;
    bool ret = (m_event_loop->StartLoop(bbt::pollevent::EventLoopOpt::LOOP_NONBLOCK) == 0);

    std::queue<std::shared_ptr<CoPollEvent>> m_swap_queue;
    {
        std::unique_lock<std::mutex> _(m_custom_event_active_queue_mutex);
        m_swap_queue.swap(m_custom_event_active_queue);
    }

    if (!m_swap_queue.empty())
        ret = true;

    /* 通知触发的自定义事件 */
    while (!m_swap_queue.empty())
    {
        auto item = m_swap_queue.front();
        item->Trigger(POLL_EVENT_CUSTOM);
        m_swap_queue.pop();
        std::unique_lock<std::mutex> _(m_custom_event_active_queue_mutex);
        Assert(m_safe_active_set.erase(item) > 0);
    }

    return ret;
}

int CoPoller::NotifyCustomEvent(std::shared_ptr<CoPollEvent> event)
{
    std::unique_lock<std::mutex> _(m_custom_event_active_queue_mutex);
    // AssertWithInfo(m_safe_active_set.find(event) != m_safe_active_set.end(), "有bug，这里不应该有重复的事件");
    if (m_safe_active_set.find(event) != m_safe_active_set.end())
        return -1;

    m_safe_active_set.insert(event);
    m_custom_event_active_queue.push(event);

    return 0;
}

int64_t CoPoller::GetTime()
{
    return m_event_loop->GetTime();
}

}