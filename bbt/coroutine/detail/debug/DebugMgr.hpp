#pragma once
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail
{

class DebugMgr
{
public:
    static std::unique_ptr<DebugMgr>& GetInstance();

    void OnEvent_ResumeCo(std::shared_ptr<Coroutine> co);
    void OnEvent_YieldCo(std::shared_ptr<Coroutine> co);
    void Check_IsResumedCo(CoroutineId id);

    void OnEvent_RegistEvent(std::shared_ptr<IPollEvent> event);
    void OnEvent_TriggerEvent(CoPollEventId id);
    void Check_Trigger(CoPollEventId id);
private:
    std::map<CoroutineId, std::shared_ptr<Coroutine>>   m_co_map;
    std::mutex                                          m_co_map_mtx;

    std::map<CoPollEventId, std::shared_ptr<IPollEvent>>    m_event_map;
    std::mutex                                              m_event_map_mtx;
};

} // namespace bbt::coroutine::detail