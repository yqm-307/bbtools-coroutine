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
    bool Check_IsResumedCo(CoroutineId id);
private:
    std::map<CoroutineId, std::shared_ptr<Coroutine>>   m_co_map;
    std::mutex                                          m_co_map_mtx;
};

} // namespace bbt::coroutine::detail