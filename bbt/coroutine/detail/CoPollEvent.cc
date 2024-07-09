#include <stdio.h>
#include <fcntl.h>
#include <bbt/base/assert/Assert.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/base/bits/BitUtil.hpp>
#include <bbt/base/Logger/DebugPrint.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>

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

CoPollEvent::SPtr CoPollEvent::Create(std::shared_ptr<Coroutine> coroutine, const CoPollEventCallback& cb)
{
    return std::make_shared<CoPollEvent>(coroutine, cb);
}

CoPollEvent::CoPollEvent(std::shared_ptr<Coroutine> coroutine, const CoPollEventCallback& cb):
    m_coroutine(coroutine),
    m_onevent_callback(cb)
{
    Assert(m_coroutine != nullptr);
    Assert(m_onevent_callback != nullptr);
    m_run_status = CoPollEventStatus::POLLEVENT_INITED;
}

CoPollEvent::~CoPollEvent()
{
}

void CoPollEvent::Trigger(short trigger_events)
{
    /**
     * 触发事件实际操作由创建者定义，实现完全解耦。
     * 
     * 对于CoPoller、CoPollEvent来说，都不需要关心事件完成后
     * 到底执行什么样的操作了，只由外部创建者定义。
     */
    /* 取消所有系统fd事件并释放资源 */

    if (m_run_status != CoPollEventStatus::POLLEVENT_LISTEN)
        return;

    if (_CannelAllFdEvent() != 0)
        g_bbt_warn_print;

    m_run_status = CoPollEventStatus::POLLEVENT_TRIGGER;

    if (m_onevent_callback != nullptr) {
        m_onevent_callback(shared_from_this(), trigger_events, m_custom_key);
    }

    _OnFinal();
}

void CoPollEvent::_OnFinal()
{
    m_run_status = CoPollEventStatus::POLLEVENT_FINAL;
}

int CoPollEvent::InitFdEvent(int fd, short events, int timeout)
{
    int ret = 0;
    if (m_run_status >= CoPollEventStatus::POLLEVENT_LISTEN || timeout < 0)
        return -1;
    
    m_timeout = timeout;
    m_event = g_bbt_poller->CreateEvent(fd, events, [this](std::shared_ptr<bbt::pollevent::Event>, short events){
        Trigger(events);
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
    /**
     * 目前不支持同时注册可读、可写事件
     */
    // Assert( ! (m_type & PollEventType::POLL_EVENT_READABLE && m_type & PollEventType::POLL_EVENT_WRITEABLE) );

    if (m_event != nullptr && (_RegistFdEvent() != 0))
        return -1;

    /* 自定义事件 */
    if (m_has_custom_event && (_RegistCustomEvent() != 0))
        return -1;

    _OnListen();
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

int CoPollEvent::_RegistCustomEvent()
{
    return 0;
}

int CoPollEvent::_RegistFdEvent()
{
    return m_event->StartListen(m_timeout);
}


int CoPollEvent::_CannelAllFdEvent()
{
    int ret = 0;
    if (m_run_status != CoPollEventStatus::POLLEVENT_LISTEN)
        return -1;
    
    if (m_event != nullptr) {
        ret = m_event->CancelListen();
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
    return m_run_status;
}


}