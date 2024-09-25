#include <stdio.h>
#include <fcntl.h>
#include <bbt/base/assert/Assert.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/base/bits/BitUtil.hpp>
#include <bbt/base/Logger/DebugPrint.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/utils/DebugPrint.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/debug/DebugMgr.hpp>

namespace bbt::coroutine::detail
{

int TransformToPollEventType(short pollevent_type, bool has_custom)
{
    int ret = 0x0;
    if (pollevent_type & pollevent::EventOpt::READABLE)
        ret |= PollEventType::POLL_EVENT_READABLE;
    
    if (pollevent_type & pollevent::EventOpt::WRITEABLE)
        ret |= PollEventType::POLL_EVENT_WRITEABLE;

    if (pollevent_type & pollevent::EventOpt::TIMEOUT)
        ret |= PollEventType::POLL_EVENT_TIMEOUT;
    
    if (has_custom)
        ret |= PollEventType::POLL_EVENT_CUSTOM;
    
    return ret;
}

CoPollEvent::SPtr CoPollEvent::Create(CoroutineId id, const CoPollEventCallback& cb)
{
    return std::make_shared<CoPollEvent>(id, cb);
}

CoPollEvent::CoPollEvent(CoroutineId id, const CoPollEventCallback& cb):
    m_co_id(id),
    m_onevent_callback(cb),
    m_event_id(_GenerateId())
{
    Assert(m_onevent_callback != nullptr);
    m_run_status = CoPollEventStatus::POLLEVENT_INITED;
}

CoPollEvent::~CoPollEvent()
{
}

int CoPollEvent::Trigger(short trigger_events)
{
    /**
     * 触发事件实际操作由创建者定义，实现完全解耦。
     * 
     * 对于CoPoller、CoPollEvent来说，都不需要关心事件完成后
     * 到底执行什么样的操作了，只由外部创建者定义。
     */

    int expect = CoPollEventStatus::POLLEVENT_LISTEN;
    if (!m_run_status.compare_exchange_strong(expect, CoPollEventStatus::POLLEVENT_TRIGGER))
        return -1;


    if (_CannelAllFdEvent() != 0)
        g_bbt_dbgp_full("");

#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_TriggerCoPollEvent();
#endif
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    // g_bbt_dbgmgr->Check_Trigger(GetId());
    g_bbt_dbgmgr->OnEvent_TriggerEvent(shared_from_this());
#endif

    if (m_onevent_callback != nullptr) {
        int event = TransformToPollEventType(trigger_events, trigger_events & POLL_EVENT_CUSTOM);
        AssertWithInfo(event > 0, "may be has a bug! trigger event must greater then 0!"); // 事件触发必须有原因
        m_onevent_callback(shared_from_this(), trigger_events, m_custom_key);
    }

    _OnFinal();
    return 0;
}

void CoPollEvent::_OnFinal()
{
    m_run_status = CoPollEventStatus::POLLEVENT_FINAL;
}

CoPollEventId CoPollEvent::_GenerateId()
{
    static std::atomic_uint64_t _id{0};
    return ++_id;
}

int CoPollEvent::InitFdEvent(int fd, short events, int timeout)
{
    int ret = 0;
    if (m_run_status >= CoPollEventStatus::POLLEVENT_LISTEN || timeout < 0)
        return -1;
    
    m_timeout = timeout;
    auto weakthis = weak_from_this();
    m_event = g_bbt_poller->CreateEvent(fd, events, [weakthis](std::shared_ptr<bbt::pollevent::Event>, short events){
        Assert(!weakthis.expired());
        auto pthis = weakthis.lock();
        pthis->Trigger(events);
    });

    return ret;
}

int CoPollEvent::InitCustomEvent(int key, void* args)
{
    if (m_run_status >= CoPollEventStatus::POLLEVENT_LISTEN || m_has_custom_event)
        return -1;

    m_has_custom_event = true;
    m_custom_key = key;
    // m_type |= PollEventType::POLL_EVENT_CUSTOM;
    return 0;
}

int CoPollEvent::Regist()
{

#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_RegistEvent(shared_from_this());
#endif

    if (m_event != nullptr && (_RegistFdEvent() != 0)) {
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_TriggerEvent(shared_from_this());
#endif
        return -1;
    }

    _OnListen();
    std::string event = m_event == nullptr ? "-1" : std::to_string(m_event->GetEvents());
    g_bbt_dbgp_full(("[CoEvent:Regist] co=" + std::to_string(m_co_id) +
                                     " event=" + event +
                                     " id=" + std::to_string(GetId()) +
                                     " customkey=" + std::to_string(m_custom_key)).c_str());
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_RegistCoPollEvent();
#endif


    return 0;
}

void CoPollEvent::_OnListen()
{
    int ret = 0;
    m_run_status = CoPollEventStatus::POLLEVENT_LISTEN;
}

int CoPollEvent::UnRegist()
{
    int ret = 0;

    /* 只能对监听中的任务执行操作 */
    if (m_run_status != CoPollEventStatus::POLLEVENT_LISTEN)
        return -1;

    if (ret == 0)
    {
        m_event->CancelListen();
        m_run_status = CoPollEventStatus::POLLEVENT_CANNEL;
    }

    return ret;
}

// int CoPollEvent::_RegistCustomEvent()
// {
    // return 0;
// }

int CoPollEvent::_RegistFdEvent()
{
    return m_event->StartListen((m_timeout < 0 ? 0 : m_timeout));
}


int CoPollEvent::_CannelAllFdEvent()
{
    int ret = 0;
    
    if (m_event != nullptr) {
        m_event = nullptr;
    }

    return ret;
}

int CoPollEvent::GetEvent() const
{
    short event = m_event->GetEvents();
    return TransformToPollEventType(event, m_has_custom_event);
}

bool CoPollEvent::IsListening() const
{
    return (m_run_status == CoPollEventStatus::POLLEVENT_LISTEN);
}

bool CoPollEvent::IsFinal() const
{
    return (m_run_status == CoPollEventStatus::POLLEVENT_FINAL);
}

CoPollEventStatus CoPollEvent::GetStatus() const
{
    return (CoPollEventStatus)m_run_status.load();
}

CoPollEventId CoPollEvent::GetId() const
{
    return m_event_id;
}

int CoPollEvent::GetFd() const
{
    return m_event->GetSocket();
}

int64_t CoPollEvent::GetTimeout() const
{
    return m_event->GetTimeoutMs();
}


}