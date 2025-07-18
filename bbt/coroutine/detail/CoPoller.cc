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
    int before_num = m_event_loop->GetEventNum();
    bool ret = (m_event_loop->StartLoop(bbt::pollevent::EventLoopOpt::LOOP_NONBLOCK) == 0);

    return (ret && m_event_loop->GetEventNum() != before_num);
}

int CoPoller::NotifyCustomEvent(std::shared_ptr<CoPollEvent> event)
{
    Assert(event != nullptr);
    return event->Trigger(POLL_EVENT_CUSTOM);
}

std::shared_ptr<bbt::pollevent::EventLoop> CoPoller::GetEventLoop() const
{
    return m_event_loop;
}

}