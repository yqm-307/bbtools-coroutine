#include <bbt/pollevent/Event.hpp>
#include <bbt/coroutine/detail/CoEventBase.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/utils/DebugPrint.hpp>

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
    m_event = g_bbt_poller->CreateEvent(fd, events, [=](std::shared_ptr<bbt::pollevent::Event>, short events){
        Trigger(events, POLL_EVENT_CUSTOM_DEFAULT);
    });

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

}