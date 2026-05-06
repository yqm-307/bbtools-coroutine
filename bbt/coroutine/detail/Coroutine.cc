#include <atomic>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/Trace.hpp>
#include <bbt/coroutine/utils/DebugPrint.hpp>
#include <bbt/coroutine/detail/debug/DebugMgr.hpp>

namespace bbt::coroutine::detail
{

typedef bbt::pollevent::EventOpt EventOpt;

CoroutineId Coroutine::_GenCoroutineId()
{
    static std::atomic_uint64_t _generate_id{BBT_COROUTINE_INVALID_COROUTINE_ID};
    return (++_generate_id);
}

Coroutine::Ptr Coroutine::Create(int stack_size, const CoroutineCallback& co_func, bool need_protect, std::string desc)
{
    return new Coroutine(stack_size, co_func, need_protect, std::move(desc));
}

Coroutine::Coroutine(int stack_size, const CoroutineCallback& co_func, bool need_protect, std::string desc):
    m_context(stack_size, [=](){co_func(); m_run_status.store(CoroutineStatus::CO_FINAL, std::memory_order_release); }, need_protect),
    m_id(_GenCoroutineId()),
    m_created_at_us(bbt::core::clock::gettime_mono<>()),
    m_desc(std::move(desc))
{
    m_run_status.store(CoroutineStatus::CO_RUNNABLE, std::memory_order_release);
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_CreateCoroutine();
#endif
    g_bbt_trace->OnCoroutineCreate(this);
}


Coroutine::~Coroutine()
{
    // Assert(m_bind_processer_id != BBT_COROUTINE_INVALID_PROCESSER_ID);
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_DestoryCoroutine();
#endif
}

void Coroutine::Resume()
{
    auto status = m_run_status.load(std::memory_order_acquire);
    Assert(status == CoroutineStatus::CO_RUNNABLE || status == CoroutineStatus::CO_SUSPEND);
    m_run_status.store(CoroutineStatus::CO_RUNNING, std::memory_order_release);
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_ResumeCo(shared_from_this());
#endif
    g_bbt_dbgp_full(("[Coroutine::Resume] co=" + std::to_string(GetId())).c_str());
    m_context.Resume();
    if (m_run_status.load(std::memory_order_acquire) == CoroutineStatus::CO_FINAL)
        g_bbt_trace->OnCoroutineFinish(this);
}

void Coroutine::Yield()
{
    Assert(m_run_status.load(std::memory_order_acquire) == CoroutineStatus::CO_RUNNING);
    m_run_status.store(CoroutineStatus::CO_SUSPEND, std::memory_order_release);
    m_last_resume_reason = TRACE_REASON_YIELD;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_YieldCo(shared_from_this());
#endif
    g_bbt_dbgp_full(("[Coroutine::Yield] co=" + std::to_string(GetId())).c_str());
    g_bbt_trace->OnCoroutineYield(this, m_last_processer_id, TRACE_REASON_YIELD);
    m_context.Yield();
}

int Coroutine::YieldWithCallback(const CoroutineOnYieldCallback& cb)
{
    /**
     * 确保协程挂起后，才可以触发事件
     */
    Assert(m_run_status.load(std::memory_order_acquire) == CoroutineStatus::CO_RUNNING);
    m_run_status.store(CoroutineStatus::CO_SUSPEND, std::memory_order_release);
    m_last_resume_reason = TRACE_REASON_EVENT;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_YieldCo(shared_from_this());
#endif
    g_bbt_dbgp_full(("[Coroutine::YieldWithCallback] co=" + std::to_string(GetId())).c_str());
    int ret = m_context.YieldWithCallback(cb);
    if (ret != 0)
        m_run_status.store(CoroutineStatus::CO_RUNNING, std::memory_order_release);
    else
        g_bbt_trace->OnCoroutineYield(this, m_last_processer_id, TRACE_REASON_EVENT);

    return ret;
}

void Coroutine::YieldAndPushGCoQueue()
{
    Assert(YieldWithCallback([this](){
        g_scheduler->OnActiveCoroutine(CO_PRIORITY_NORMAL, this, TRACE_REASON_YIELD);
        return true;
    }) == 0);
}


CoroutineId Coroutine::GetId() noexcept
{
    return m_id;
}

CoroutineStatus Coroutine::GetStatus() const noexcept
{
    return m_run_status.load(std::memory_order_acquire);
}

int Coroutine::GetLastResumeReason() const noexcept
{
    return m_last_resume_reason;
}

ProcesserId Coroutine::GetLastProcesserId() const noexcept
{
    return m_last_processer_id;
}

void Coroutine::OnException() noexcept
{
    m_run_status.store(CoroutineStatus::CO_FINAL, std::memory_order_release);
    if (m_await_event) {
        m_await_event->UnRegist();
        m_await_event = nullptr;
    }
    g_bbt_trace->OnCoroutineFinish(this);
}

void Coroutine::FinalizeForTeardown() noexcept
{
    m_run_status.store(CoroutineStatus::CO_FINAL, std::memory_order_release);
    if (m_await_event) {
        m_await_event->UnRegist();
        m_await_event = nullptr;
    }
    m_last_resume_event = -1;
    g_bbt_trace->OnCoroutineTeardown(this);
}

int Coroutine::YieldUntilTimeout(int ms)
{
    Assert(m_await_event == nullptr);
    m_await_event = CoPollEvent::Create(GetId(), [&](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    if (m_await_event->InitFdEvent(-1, EventOpt::TIMEOUT, ms) != 0)
        return -1;

    return YieldWithCallback([this](){
        return (m_await_event->Regist() == 0);
    });
}

std::shared_ptr<CoPollEvent> Coroutine::RegistCustom(int key)
{
    if (m_await_event != nullptr)
        return nullptr;
    
    m_await_event = CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    if (m_await_event->InitCustomEvent(key, NULL) != 0)
        return nullptr;
    
    return m_await_event;
}

std::shared_ptr<CoPollEvent> Coroutine::RegistCustom(int key, int timeout_ms)
{
    if (m_await_event != nullptr)
        return nullptr;
    
    m_await_event = CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    if (m_await_event->InitCustomEvent(key, NULL) != 0)
        return nullptr;
    
    if (m_await_event->InitFdEvent(-1, EventOpt::TIMEOUT, timeout_ms) != 0)
        return nullptr;
    
    return m_await_event;
}

int Coroutine::YieldUntilFdReadable(int fd)
{
    Assert(m_await_event == nullptr);
    m_await_event = CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    if (m_await_event->InitFdEvent(fd, EventOpt::READABLE | EventOpt::FINALIZE, 0) != 0)
        return -1;
    
    return YieldWithCallback([this](){
        return (m_await_event->Regist() == 0);
    });
}

int Coroutine::YieldUntilFdReadable(int fd, int timeout_ms)
{
    Assert(m_await_event == nullptr);
    
    m_await_event = CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    if (m_await_event->InitFdEvent(fd, EventOpt::READABLE | EventOpt::TIMEOUT | EventOpt::FINALIZE, timeout_ms) != 0)
        return -1;

    return YieldWithCallback([this](){
        return (m_await_event->Regist() == 0);
    });
}

int Coroutine::YieldUntilFdWriteable(int fd)
{
    Assert(m_await_event == nullptr);
    m_await_event = CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    if (m_await_event->InitFdEvent(fd, EventOpt::WRITEABLE | EventOpt::FINALIZE, 0) != 0)
        return -1;

    return YieldWithCallback([this](){
        return (m_await_event->Regist() == 0);
    });
}

int Coroutine::YieldUntilFdWriteable(int fd, int timeout_ms)
{
    Assert(m_await_event == nullptr);
    m_await_event = CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    if (m_await_event->InitFdEvent(fd, EventOpt::WRITEABLE | EventOpt::TIMEOUT | EventOpt::FINALIZE, timeout_ms) != 0)
        return -1;

    return YieldWithCallback([this](){
        return (m_await_event->Regist() == 0);
    });
}

int Coroutine::YieldUntilFdEx(int fd, short events, int timeout_ms)
{
    Assert(m_await_event == nullptr);
    m_await_event = CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    /* 绝对不可以反复触发 */
    if (m_await_event->InitFdEvent(fd, events & ~pollevent::EventOpt::PERSIST, timeout_ms) != 0)
        return -1;

    return YieldWithCallback([this](){
        return (m_await_event->Regist() == 0);
    });
}


void Coroutine::OnCoPollEvent(int event, int custom_key)
{
    CoroutinePriority priority = CO_PRIORITY_NORMAL;

    Assert(m_await_event != nullptr);

    m_last_resume_event = event;
    m_last_resume_reason = TRACE_REASON_EVENT;
    m_run_status.store(CoroutineStatus::CO_RUNNABLE, std::memory_order_release);

    // 先取消事件，然后push到全局队列中
    g_bbt_dbgp_full(("[CoEvent:Trigger] co=" + std::to_string(GetId()) + " trigger_event=" + std::to_string(event) + " id=" + std::to_string(m_await_event->GetId()) + " customkey=" + std::to_string(custom_key)).c_str());
    m_await_event = nullptr;

    // 超时任务优先级高一些
    if (event & EventOpt::TIMEOUT)
        priority = CO_PRIORITY_CRITICAL;

    g_scheduler->OnActiveCoroutine(priority, this, TRACE_REASON_EVENT);

}

int Coroutine::GetLastResumeEvent() const noexcept
{
    return m_last_resume_event;
}

size_t Coroutine::GetStackSize() const noexcept
{
    return m_context.GetStackSize();
}

const std::string& Coroutine::GetDesc() const noexcept
{
    return m_desc;
}

uint64_t Coroutine::GetCreatedAtUs() const noexcept
{
    return m_created_at_us;
}

bool Coroutine::IsTraceEnabled() const noexcept
{
    return m_trace_enabled;
}

void Coroutine::SetLastResumeReason(int reason) noexcept
{
    m_last_resume_reason = reason;
}

void Coroutine::SetLastProcesserId(ProcesserId id) noexcept
{
    m_last_processer_id = id;
}

void Coroutine::SetTraceEnabled(bool enabled) noexcept
{
    m_trace_enabled = enabled;
}

CoroutineTraceMeta Coroutine::BuildTraceMeta() const
{
    CoroutineTraceMeta meta;
    meta.id = m_id;
    meta.desc = m_desc;
    meta.created_at_us = m_created_at_us;
    meta.status = GetStatus();
    meta.last_resume_reason = m_last_resume_reason;
    meta.last_resume_event = m_last_resume_event;
    meta.last_processer_id = m_last_processer_id;
    meta.traced = m_trace_enabled;
    return meta;
}

}
