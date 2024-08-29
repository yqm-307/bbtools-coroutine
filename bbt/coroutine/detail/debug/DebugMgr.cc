#include <bbt/coroutine/detail/debug/DebugMgr.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>


namespace bbt::coroutine::detail
{

std::unique_ptr<DebugMgr>& DebugMgr::GetInstance()
{
    static std::unique_ptr<DebugMgr> _inst = nullptr;
    if (_inst == nullptr)
        _inst = std::unique_ptr<DebugMgr>(new DebugMgr());
    
    return _inst;
}

void DebugMgr::OnEvent_ResumeCo(std::shared_ptr<Coroutine> co)
{
    std::lock_guard<std::mutex> _(m_co_map_mtx);
    AssertWithInfo(m_co_map.find(co->GetId()) == m_co_map.end(), "[error] repeat resume coroutine!");
    m_co_map.insert(std::make_pair(co->GetId(), co));
}

void DebugMgr::OnEvent_YieldCo(std::shared_ptr<Coroutine> co)
{
    std::lock_guard<std::mutex> _(m_co_map_mtx);
    AssertWithInfo(m_co_map.erase(co->GetId()) > 0, "[error] repeat yield coroutine!");
}

void DebugMgr::Check_IsResumedCo(CoroutineId id)
{
    std::lock_guard<std::mutex> _(m_co_map_mtx);
    Assert(m_co_map.find(id) == m_co_map.end());
}

void DebugMgr::OnEvent_RegistEvent(std::shared_ptr<CoPollEvent> event)
{
    std::lock_guard<std::mutex> _(m_co_map_mtx);
    AssertWithInfo(m_event_map.find(event->GetId()) == m_event_map.end(), "[error] repeat regist event!");
    m_event_map.insert(std::make_pair(event->GetId(), event));
}

void DebugMgr::OnEvent_TriggerEvent(std::shared_ptr<CoPollEvent> event)
{
    std::lock_guard<std::mutex> _(m_co_map_mtx);
    AssertWithInfo(m_event_map.erase(event->GetId()) > 0, "[error] trigger event no registed or triggered!");
}

void DebugMgr::Check_Trigger(CoPollEventId id)
{
    std::lock_guard<std::mutex> _(m_co_map_mtx);
    Assert(m_event_map.find(id) != m_event_map.end());
}

} // namespace bbt::coroutine::detail