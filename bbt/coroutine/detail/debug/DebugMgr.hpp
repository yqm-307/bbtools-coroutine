#pragma once
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail
{

class DebugMgr
{
public:
    static std::unique_ptr<DebugMgr>& GetInstance();

    void OnEvent_ResumeCo(Coroutine* co);
    void OnEvent_YieldCo(Coroutine* co);
    void Check_IsResumedCo(CoroutineId id);

    void OnEvent_RegistEvent(std::shared_ptr<CoPollEvent> event);
    void OnEvent_TriggerEvent(std::shared_ptr<CoPollEvent> event);
    void Check_Trigger(CoPollEventId id);
private:
    std::map<CoroutineId, Coroutine*>   m_co_map;
    std::mutex                                          m_co_map_mtx;

    std::map<CoPollEventId, std::shared_ptr<CoPollEvent>>   m_event_map;
    std::mutex                                              m_event_map_mtx;
};

} // namespace bbt::coroutine::detail