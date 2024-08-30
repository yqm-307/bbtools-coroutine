#include <bbt/base/assert/Assert.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/interface/IPollEvent.hpp>

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
    return (m_event_loop->StartLoop(bbt::pollevent::EventLoopOpt::LOOP_NONBLOCK) == 0);
}

////////////////////////////////////////////////////// new api ///////////////////////////////////////////////


int CoPoller::UnRegist(CoPollEventId id)
{
    std::lock_guard<std::mutex> _(m_event_map_mtx);
    auto earse_num = m_event_map.erase(id);
    return ((earse_num > 0) ? 0 : -1);
}

std::pair<int, CoPollEventId> CoPoller::Regist(std::shared_ptr<IPollEvent> event)
{
    CoroutineId eventid = 0;

    if (event == nullptr)
        return {-1, eventid};
    
    eventid = event->GetId();
    auto [_, succ] = m_event_map.insert(std::make_pair(eventid, event));

    return {(succ ? 0 : -1), eventid};
}

int CoPoller::Notify(CoPollEventId eventid, short trigger_event, CoPollEventCustom custom_key)
{
    /* 保证了CoPollEvent触发是唯一的 */
    std::lock_guard<std::mutex> _(m_event_map_mtx);

    auto it = m_event_map.find(eventid);
    if (it == m_event_map.end())
        return -1;
    
    it = m_event_map.erase(it);
    if (it->second->Trigger(trigger_event, custom_key) != 0)
        return -1;
    
    return 0;
}

}