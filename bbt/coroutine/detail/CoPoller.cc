#include <bbt/core/util/Assert.hpp>
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
    /* 首个 backend：bbt::pollevent::EventLoop（ASIO）。不是 epoll/timerfd。 */
    auto* base = new bbt::pollevent::detail::EventBase(
        bbt::pollevent::detail::EventBaseConfigFlag::NO_CACHE_TIME |
        bbt::pollevent::detail::EventBaseConfigFlag::PRECISE_TIMER);
    
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
    /* 驱动 EventLoop，不直接 epoll_wait。积压 Event 在本线程销毁。 */
    {
        std::lock_guard<std::mutex> lock(m_deferred_mutex);
        m_deferred_destroy.clear();
    }
    return (m_event_loop->StartLoop(bbt::pollevent::EventLoopOpt::LOOP_NONBLOCK) == 0);
}

int CoPoller::NotifyCustomEvent(std::shared_ptr<CoPollEvent> event)
{
    Assert(event != nullptr);
    return event->Trigger(POLL_EVENT_CUSTOM);
}

void CoPoller::DeferDestroyEvent(std::shared_ptr<bbt::pollevent::Event> event)
{
    if (event == nullptr) return;
    std::lock_guard<std::mutex> lock(m_deferred_mutex);
    m_deferred_destroy.push_back(std::move(event));
}

void CoPoller::FlushDeferredEvents()
{
    std::vector<std::shared_ptr<bbt::pollevent::Event>> deferred;
    {
        std::lock_guard<std::mutex> lock(m_deferred_mutex);
        deferred.swap(m_deferred_destroy);
    }
    /* 在锁外析构，避免 Event::CancelListen 回调路径反向获取本锁。 */
}

std::shared_ptr<bbt::pollevent::EventLoop> CoPoller::GetEventLoop() const
{
    return m_event_loop;
}

}