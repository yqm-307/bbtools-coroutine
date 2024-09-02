#include <bbt/pollevent/Event.hpp>
#include <bbt/coroutine/detail/CoEventBase.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/utils/DebugPrint.hpp>
#include <bbt/coroutine/detail/debug/DebugMgr.hpp>

namespace bbt::coroutine::detail
{

CoPollEventId CoEventBase::_GenerateId()
{
    static std::atomic_uint64_t _id{0};
    return (++_id);
}

CoPollEventId CoEventBase::GetId() const
{
    return m_id;
}

std::shared_ptr<Coroutine> CoEventBase::GetWaitCo() const
{
    return m_wait_coroutine;
}


CoEventBase::CoEventBase():
    m_id(_GenerateId()),
    m_wait_coroutine(g_bbt_tls_coroutine_co)
{
    AssertWithInfo(m_wait_coroutine != nullptr, "non-coroutine!");
}

CoEventBase::~CoEventBase()
{
    m_wait_coroutine = nullptr;
}

int CoEventBase::InitFdEvent(int fd, short events, int ms)
{
    m_event = g_bbt_poller->CreateEvent(GetId(), fd, events);

    m_timeout = ms;

    return 0;
}

void CoEventBase::InitCustomEvent(CoPollEventCustom custom_key)
{
    m_custom_key = custom_key;
}

int CoEventBase::Regist()
{
    if (m_event != nullptr && (m_event->StartListen(m_timeout) != 0))
        return -1;

    std::string event = m_event == nullptr ? "-1" : std::to_string(m_event->GetEvents());
    g_bbt_dbgp_full(("[CoEvent:Regist] co=" + std::to_string(GetWaitCo()->GetId()) +
                                     " event=" + event +
                                     " id=" + std::to_string(GetId()) +
                                     " customkey=" + std::to_string(m_custom_key)).c_str());

    return 0;
}

int CoEventBase::Trigger(short trigger_events, int customkey)
{

#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_TriggerEvent(GetId());
#endif
    // 调用实例的具体实现
    return OnNotify(trigger_events, customkey);
}


}