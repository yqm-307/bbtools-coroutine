#include <stdio.h>
#include <fcntl.h>
#include <exception>
#include <string>
#include <bbt/core/util/Assert.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/log/DebugPrint.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/utils/DebugPrint.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/debug/DebugMgr.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

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
    m_event_id(_GenerateId()),
    m_onevent_callback(cb)
{
    Assert(m_onevent_callback != nullptr);
}

CoPollEvent::~CoPollEvent()
{
    // 已 arm 的 Event 只能在 PollOnce 线程析构，避免与 event loop 竞态。
    _CannelAllFdEvent();
}

int CoPollEvent::Trigger(short trigger_events)
{
    if (trigger_events == 0)
        return -1;

    uint64_t state = m_state.load(std::memory_order_acquire);
    for (;;)
    {
        const auto phase = GetCoPollEventPhase(state);
        if (phase == CoPollEventPhase::INITED ||
            phase == CoPollEventPhase::ARMING ||
            phase == CoPollEventPhase::ARMED)
        {
            const uint64_t pending = PackCoPollEventState(CoPollEventPhase::PENDING, trigger_events);
            if (m_state.compare_exchange_weak(state, pending,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire))
                return 0;
            continue;
        }

        if (phase != CoPollEventPhase::PARKED)
            return -1;

        const uint64_t triggering = PackCoPollEventState(CoPollEventPhase::TRIGGERING,
                                                           trigger_events);
        if (m_state.compare_exchange_weak(state, triggering,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
            return _Complete(trigger_events);
    }
}

CoPollEventId CoPollEvent::_GenerateId()
{
    static std::atomic_uint64_t _id{0};
    return ++_id;
}

int CoPollEvent::InitFdEvent(int fd, short events, int timeout)
{
    if (GetCoPollEventPhase(m_state.load(std::memory_order_acquire)) != CoPollEventPhase::INITED ||
        timeout < 0 || m_event != nullptr)
        return -1;

    m_fd = fd;
    m_listen_events = events;
    m_timeout = timeout;
    auto weakthis = weak_from_this();
    m_event = g_bbt_poller->CreateEvent(fd, events, [weakthis](int fd, short events, bbt::pollevent::EventId eventid){
        if (weakthis.expired()) return;
        auto pthis = weakthis.lock();
        if (pthis == nullptr) return;
        pthis->Trigger(events);
    });

    return m_event == nullptr ? -1 : 0;
}

int CoPollEvent::InitCustomEvent(int key, void* args)
{
    BBTATTR_COMM_UNUSED void* unused_args = args;
    if (GetCoPollEventPhase(m_state.load(std::memory_order_acquire)) != CoPollEventPhase::INITED ||
        m_has_custom_event)
        return -1;

    m_has_custom_event = true;
    m_custom_key = key;
    return 0;
}

int CoPollEvent::Regist()
{
    uint64_t state = m_state.load(std::memory_order_acquire);
    if (GetCoPollEventPhase(state) == CoPollEventPhase::PENDING) {
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
        g_bbt_dbgmgr->OnEvent_RegistEvent(shared_from_this());
#endif
        return 0;
    }

    uint64_t expected = PackCoPollEventState(CoPollEventPhase::INITED);
    if (!m_state.compare_exchange_strong(expected, PackCoPollEventState(CoPollEventPhase::ARMING),
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire))
    {
        if (GetCoPollEventPhase(expected) == CoPollEventPhase::PENDING) {
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
            g_bbt_dbgmgr->OnEvent_RegistEvent(shared_from_this());
#endif
            return 0;
        }
        return -1;
    }

    auto keep_alive = shared_from_this();
    int regist_ret = 0;
    try
    {
        if (m_event != nullptr)
            regist_ret = _RegistFdEvent();
    }
    catch (...)
    {
        // StartListen() 抛出时仍由 ARMING 持有者收敛状态并转移 Event。
        state = m_state.load(std::memory_order_acquire);
        while (GetCoPollEventPhase(state) == CoPollEventPhase::ARMING) {
            if (m_state.compare_exchange_weak(state, PackCoPollEventState(CoPollEventPhase::CANCELLED),
                                              std::memory_order_acq_rel,
                                               std::memory_order_acquire))
                break;
        }
        _CannelAllFdEvent();
        if (GetCoPollEventPhase(state) == CoPollEventPhase::PENDING) {
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
            g_bbt_dbgmgr->OnEvent_RegistEvent(keep_alive);
#endif
            return 0;
        }
        return -1;
    }

    if (regist_ret != 0) {
        state = m_state.load(std::memory_order_acquire);
        while (GetCoPollEventPhase(state) == CoPollEventPhase::ARMING) {
            if (m_state.compare_exchange_weak(state, PackCoPollEventState(CoPollEventPhase::CANCELLED),
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire))
                break;
        }
        _CannelAllFdEvent();
        if (GetCoPollEventPhase(state) == CoPollEventPhase::PENDING) {
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
            g_bbt_dbgmgr->OnEvent_RegistEvent(keep_alive);
#endif
            return 0;
        }
        return -1;
    }

    expected = PackCoPollEventState(CoPollEventPhase::ARMING);
    if (!m_state.compare_exchange_strong(expected, PackCoPollEventState(CoPollEventPhase::ARMED),
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire))
    {
        _CannelAllFdEvent();
        if (GetCoPollEventPhase(expected) == CoPollEventPhase::PENDING) {
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
            g_bbt_dbgmgr->OnEvent_RegistEvent(keep_alive);
#endif
            return 0;
        }
        return -1;
    }

#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_RegistEvent(keep_alive);
#endif
    g_bbt_dbgp_full(("[CoEvent:Regist] co=" + std::to_string(m_co_id) +
                                     " event=" + std::to_string(m_listen_events) +
                                     " id=" + std::to_string(GetId()) +
                                     " customkey=" + std::to_string(m_custom_key)).c_str());
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_RegistCoPollEvent();
#endif
    return 0;
}

bool CoPollEvent::CommitPark()
{
    /*
     * Context 已切回 Processer，注册回调也已完成；此处才是协程真正提交等待的边界。
     * ARMED 表示尚未触发，发布 PARKED 后触发方可直接完成；PENDING 表示触发已先胜，
     * 必须由当前 Processer 消费其 flags。FINAL/CANCELLED 等终态不再产生回调。
     */
    uint64_t state = m_state.load(std::memory_order_acquire);
    for (;;)
    {
        const auto phase = GetCoPollEventPhase(state);
        if (phase == CoPollEventPhase::ARMED) {
            // 发布 PARKED 是尾操作；成功后 Processer 不得再访问关联 Coroutine。
            if (m_state.compare_exchange_weak(state, PackCoPollEventState(CoPollEventPhase::PARKED),
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
                return false;
            continue;
        }

        if (phase != CoPollEventPhase::PENDING)
            return false;

        // 提前触发的 flags 已随状态字发布，只有 CAS 胜者可以执行完成回调。
        const short trigger_events = GetCoPollEventFlags(state);
        if (m_state.compare_exchange_weak(state,
                                          PackCoPollEventState(CoPollEventPhase::TRIGGERING,
                                                               trigger_events),
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
        {
            // pending 完成运行在 Processer 线程，不能让 callback 异常逃出。
            try
            {
                _Complete(trigger_events);
            }
            catch (...)
            {
            }
            return true;
        }
    }
}

int CoPollEvent::UnRegist()
{
    uint64_t state = m_state.load(std::memory_order_acquire);
    for (;;)
    {
        const auto phase = GetCoPollEventPhase(state);
        if (phase != CoPollEventPhase::INITED &&
            phase != CoPollEventPhase::ARMING &&
            phase != CoPollEventPhase::ARMED &&
            phase != CoPollEventPhase::PARKED)
            return -1;

        if (!m_state.compare_exchange_weak(state, PackCoPollEventState(CoPollEventPhase::CANCELLED),
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire))
            continue;

        // ARMING 的 Regist() 仍可能在 StartListen() 内部，资源清理由它完成。
        if (phase != CoPollEventPhase::ARMING)
            _CannelAllFdEvent();
        return 0;
    }
}

int CoPollEvent::_RegistFdEvent()
{
    return m_event->StartListen((m_timeout < 0 ? 0 : m_timeout));
}

int CoPollEvent::_CannelAllFdEvent()
{
    if (m_event != nullptr)
        g_bbt_poller->DeferDestroyEvent(std::move(m_event));
    return 0;
}

int CoPollEvent::GetEvent() const
{
    return TransformToPollEventType(m_listen_events, m_has_custom_event);
}

bool CoPollEvent::IsListening() const
{
    const auto phase = GetCoPollEventPhase(m_state.load(std::memory_order_acquire));
    return phase == CoPollEventPhase::ARMING ||
        phase == CoPollEventPhase::ARMED ||
        phase == CoPollEventPhase::PARKED;
}

bool CoPollEvent::IsFinal() const
{
    const auto phase = GetCoPollEventPhase(m_state.load(std::memory_order_acquire));
    return phase == CoPollEventPhase::FINAL || phase == CoPollEventPhase::CANCELLED;
}

CoPollEventStatus CoPollEvent::GetStatus() const
{
    switch (GetCoPollEventPhase(m_state.load(std::memory_order_acquire)))
    {
    case CoPollEventPhase::INITED:
        return CoPollEventStatus::POLLEVENT_INITED;
    case CoPollEventPhase::ARMING:
    case CoPollEventPhase::ARMED:
    case CoPollEventPhase::PARKED:
        return CoPollEventStatus::POLLEVENT_LISTEN;
    case CoPollEventPhase::PENDING:
    case CoPollEventPhase::TRIGGERING:
        return CoPollEventStatus::POLLEVENT_TRIGGER;
    case CoPollEventPhase::FINAL:
        return CoPollEventStatus::POLLEVENT_FINAL;
    case CoPollEventPhase::CANCELLED:
        return CoPollEventStatus::POLLEVENT_CANNEL;
    }
    return CoPollEventStatus::POLLEVENT_DEFAULT;
}

CoPollEventId CoPollEvent::GetId() const
{
    return m_event_id;
}

int CoPollEvent::GetFd() const
{
    return m_fd;
}

int64_t CoPollEvent::GetTimeout() const
{
    return m_timeout;
}

int CoPollEvent::_Complete(short trigger_events)
{
    auto keep_alive = shared_from_this();
    _CannelAllFdEvent();
    m_state.store(PackCoPollEventState(CoPollEventPhase::FINAL, trigger_events),
                  std::memory_order_release);

#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_TriggerCoPollEvent();
#endif
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_TriggerEvent(keep_alive);
#endif

    const auto callback = m_onevent_callback;
    const int custom_key = m_custom_key;
    std::exception_ptr pending_exception{nullptr};
    try
    {
        callback(keep_alive, trigger_events, custom_key);
    }
    catch(const std::exception& e)
    {
        if (g_bbt_coroutine_config->m_ext_coevent_exception_callback != nullptr)
            g_bbt_coroutine_config->m_ext_coevent_exception_callback(core::errcode::Errcode(e.what()));
        else
            pending_exception = std::current_exception();
    }
    catch(...)
    {
        if (g_bbt_coroutine_config->m_ext_coevent_exception_callback != nullptr)
            g_bbt_coroutine_config->m_ext_coevent_exception_callback(core::errcode::Errcode("unknown exception"));
        else
            pending_exception = std::current_exception();
    }

    if (pending_exception)
        std::rethrow_exception(pending_exception);
    return 0;
}

}
