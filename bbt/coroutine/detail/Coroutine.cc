#include <atomic>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/utils/DebugPrint.hpp>
#include <bbt/coroutine/detail/debug/DebugMgr.hpp>

namespace bbt::coroutine::detail
{

typedef bbt::pollevent::EventOpt EventOpt;

CoroutineId Coroutine::GenCoroutineId()
{
    static std::atomic_uint64_t _generate_id{BBT_COROUTINE_INVALID_COROUTINE_ID};
    return (++_generate_id);
}

Coroutine::SPtr Coroutine::Create(int stack_size, const CoroutineCallback& co_func, bool need_protect)
{
    return new Coroutine(stack_size, co_func, need_protect);
}

Coroutine::SPtr Coroutine::Create(int stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool need_protect)
{
    return new Coroutine(stack_size, co_func, co_final_cb, need_protect);
}

Coroutine::Coroutine(int stack_size, const CoroutineCallback& co_func, bool need_protect):
    m_context(stack_size, co_func, [this](){_OnCoroutineFinal();}, need_protect),
    m_id(GenCoroutineId())
{
    m_run_status = CoroutineStatus::CO_PENDING;
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_CreateCoroutine();
#endif
}

Coroutine::Coroutine(int stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool need_protect):
    m_context(stack_size, co_func, [this](){_OnCoroutineFinal();}, need_protect),
    m_id(GenCoroutineId()),
    m_co_final_callback(co_final_cb)
{
    m_run_status = CoroutineStatus::CO_PENDING;
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_CreateCoroutine();
#endif
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
    Assert(m_run_status == CoroutineStatus::CO_PENDING || m_run_status == CoroutineStatus::CO_SUSPEND);
    m_run_status = CoroutineStatus::CO_RUNNING;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_ResumeCo(shared_from_this());
#endif
    g_bbt_dbgp_full(("[Coroutine::Resume] co=" + std::to_string(GetId())).c_str());
    m_context.Resume();
}

void Coroutine::Yield()
{
    Assert(m_run_status == CoroutineStatus::CO_RUNNING);
    m_run_status = CoroutineStatus::CO_SUSPEND;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_YieldCo(shared_from_this());
#endif
    g_bbt_dbgp_full(("[Coroutine::Yield] co=" + std::to_string(GetId())).c_str());
    m_context.Yield();
}

int Coroutine::YieldWithCallback(const CoroutineOnYieldCallback& cb)
{
    /**
     * 确保协程挂起后，才可以触发事件
     */
    Assert(m_run_status == CoroutineStatus::CO_RUNNING);
    m_run_status = CoroutineStatus::CO_SUSPEND;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_YieldCo(shared_from_this());
#endif
    g_bbt_dbgp_full(("[Coroutine::YieldWithCallback] co=" + std::to_string(GetId())).c_str());
    return m_context.YieldWithCallback(cb);
}

void Coroutine::YieldAndPushGCoQueue()
{
    Assert(YieldWithCallback([this](){
        g_scheduler->OnActiveCoroutine(CO_PRIORITY_NORMAL, this);
        return true;
    }) == 0);
}


CoroutineId Coroutine::GetId()
{
    return m_id;
}

CoroutineStatus Coroutine::GetStatus()
{
    return m_run_status;
}

void Coroutine::_OnCoroutineFinal()
{
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_YieldCo(shared_from_this());
#endif
    m_run_status = CoroutineStatus::CO_FINAL;
    if (m_co_final_callback)
        m_co_final_callback();
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

void Coroutine::OnCoPollEvent(int event, int custom_key)
{
    CoroutinePriority priority = CO_PRIORITY_NORMAL;

    Assert(m_await_event != nullptr);

    m_last_resume_event = event;

    // 先取消事件，然后push到全局队列中
    g_bbt_dbgp_full(("[CoEvent:Trigger] co=" + std::to_string(GetId()) + " trigger_event=" + std::to_string(event) + " id=" + std::to_string(m_await_event->GetId()) + " customkey=" + std::to_string(custom_key)).c_str());
    m_await_event = nullptr;

    // 超时任务优先级高一些
    if (event & EventOpt::TIMEOUT)
        priority = CO_PRIORITY_CRITICAL;

    g_scheduler->OnActiveCoroutine(priority, this);

}

int Coroutine::GetLastResumeEvent()
{
    return m_last_resume_event;
}

}