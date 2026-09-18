#include <cstdio>
#include <fcntl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
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

namespace
{

void ReportCoPollEventException(CoroutineId co_id, CoPollEventId event_id,
                                std::exception_ptr eptr) noexcept
{
    const auto report = [co_id, event_id](const char* message) noexcept {
        std::fprintf(stderr, "[bbtco] unhandled coevent exception co=%llu event=%llu: %.200s\n",
                     static_cast<unsigned long long>(co_id),
                     static_cast<unsigned long long>(event_id), message);
        g_bbt_coroutine_config->m_unhandled_exception_count.fetch_add(1, std::memory_order_relaxed);
    };

    try
    {
        if (eptr != nullptr)
            std::rethrow_exception(eptr);
    }
    catch (const std::exception& e)
    {
        report(e.what());
        return;
    }
    catch (...)
    {
    }

    report("unknown exception");
}

/* #284：重试等待必须直达内核。std::this_thread::sleep_for 经 libc nanosleep，
 * 而本进程 nanosleep 已被 Hook 拦截——协程上下文会重入 YieldUntilTimeout，
 * 造成 double-wait 断言与等待状态错乱；syscall(2) 绕过全部 userspace hook。 */
void RawSleepMs(long ms) noexcept
{
    struct timespec req{ms / 1000, (ms % 1000) * 1000000L};
    while (::syscall(SYS_nanosleep, &req, &req) != 0 && errno == EINTR)
    {
    }
}

}

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
        {
            try
            {
                return _Complete(trigger_events);
            }
            catch (...)
            {
                ReportCoPollEventException(m_co_id, m_event_id, std::current_exception());
                return 0;
            }
        }
    }
}

CoPollEventId CoPollEvent::_GenerateId()
{
    static std::atomic_uint64_t _id{0};
    return ++_id;
}

/* fd → 活跃等待事件（#262）：Linux close 对 epoll 静默移除关注项，需显式唤醒 */
std::mutex CoPollEvent::s_waiters_mtx;
std::unordered_map<int, std::vector<std::weak_ptr<CoPollEvent>>> CoPollEvent::s_waiters;

void CoPollEvent::_TrackWaiter()
{
    if (m_fd < 0 || m_event == nullptr)
        return;

    std::lock_guard<std::mutex> lock(s_waiters_mtx);
    auto& vec = s_waiters[m_fd];
    vec.push_back(weak_from_this());
    // ponytail: 惰性清理，表偶尔膨胀，不单独起回收路径
    if (vec.size() > 16) {
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [](const std::weak_ptr<CoPollEvent>& w) { return w.expired(); }),
                  vec.end());
        if (vec.empty())
            s_waiters.erase(m_fd);
    }
}

void CoPollEvent::_UntrackWaiter()
{
    if (m_fd < 0)
        return;

    std::lock_guard<std::mutex> lock(s_waiters_mtx);
    auto it = s_waiters.find(m_fd);
    if (it == s_waiters.end())
        return;

    auto& vec = it->second;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                             [this](const std::weak_ptr<CoPollEvent>& w) {
                                 auto p = w.lock();
                                 return p.get() == this || p == nullptr;
                             }),
              vec.end());
    if (vec.empty())
        s_waiters.erase(it);
}

void CoPollEvent::WakeupFdWaiters(int fd)
{
    std::vector<SPtr> snapshot;
    {
        std::lock_guard<std::mutex> lock(s_waiters_mtx);
        auto it = s_waiters.find(fd);
        if (it == s_waiters.end())
            return;
        snapshot.reserve(it->second.size());
        for (auto& w : it->second)
            if (auto p = w.lock())
                snapshot.push_back(p);
    }
    // 锁外 Trigger：避免与完成路径的加锁顺序纠缠
    for (auto& p : snapshot)
        p->Trigger(p->m_listen_events != 0 ? p->m_listen_events : static_cast<short>(pollevent::EventOpt::READABLE));
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
    /* #284：asio 侧旧监听的析构延迟到驱动线程下一次 PollOnce 兑现；本协程
     * 被唤醒后若抢在兑现前为同一 fd 建新监听，epoll ADD 报 EEXIST，异常曾
     * 一路逃出杀死连接协程（客户端超时）。此处有界重试：驱动约 1ms 一拍，
     * 旧监听很快释放；仍失败则返回 -1 不抛异常，调用方按等待建立失败处理，
     * 协程存活。常态零开销，仅竞态路径短睡 worker（默认 stall 告警关闭）。 */
    for (int attempt = 0; ; ++attempt)
    {
        try
        {
            m_event = g_bbt_poller->CreateEvent(fd, events, [weakthis](int fd, short events, bbt::pollevent::EventId eventid){
                if (weakthis.expired()) return;
                auto pthis = weakthis.lock();
                if (pthis == nullptr) return;
                pthis->Trigger(events);
            });
            break;
        }
        catch (const std::exception& e)
        {
            if (attempt >= 3)
            {
                std::fprintf(stderr, "[bbtco] InitFdEvent give up co=%llu fd=%d: %.100s\n",
                             static_cast<unsigned long long>(m_co_id), fd, e.what());
                return -1;
            }
            RawSleepMs(1);
        }
    }

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

    // ARMED 起进入监听态，纳入 fd 唤醒注册表（#262）
    _TrackWaiter();

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
                ReportCoPollEventException(m_co_id, m_event_id, std::current_exception());
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
    _UntrackWaiter();
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
