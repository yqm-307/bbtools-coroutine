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

CoroutineId Coroutine::_GenCoroutineId()
{
    static std::atomic_uint64_t _generate_id{BBT_COROUTINE_INVALID_COROUTINE_ID};
    return (++_generate_id);
}

Coroutine::Ptr Coroutine::Create(int stack_size, const CoroutineCallback& co_func, bool need_protect)
{
    return new Coroutine(stack_size, co_func, need_protect);
}

Coroutine::Coroutine(int stack_size, const CoroutineCallback& co_func, bool need_protect):
    m_context(stack_size, [=](){
        try {
            co_func();
            m_yield_disposition = CoroutineYieldDisposition::FINAL;
            m_run_status = CoroutineStatus::CO_FINAL;
        } catch (...) {
            // 无 Processer TLS 时 Context 捕不到当前协程；失败必须进入终态。
            OnException();
            throw;
        }
    }, need_protect),
    m_id(_GenCoroutineId())
{
    m_run_status = CoroutineStatus::CO_RUNNABLE;
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
    Assert(m_run_status == CoroutineStatus::CO_RUNNABLE || m_run_status == CoroutineStatus::CO_SUSPEND);
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
    m_yield_disposition = CoroutineYieldDisposition::MANUAL;
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
    m_yield_disposition = _AwaitEvent() == nullptr ?
        CoroutineYieldDisposition::MANUAL : CoroutineYieldDisposition::EVENT_WAIT;
    m_run_status = CoroutineStatus::CO_SUSPEND;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_YieldCo(shared_from_this());
#endif
    g_bbt_dbgp_full(("[Coroutine::YieldWithCallback] co=" + std::to_string(GetId())).c_str());
    return m_context.YieldWithCallback(cb);
}

void Coroutine::YieldAndPushGCoQueue()
{
    Assert(m_run_status == CoroutineStatus::CO_RUNNING);
    m_yield_disposition = CoroutineYieldDisposition::READY;
    m_run_status = CoroutineStatus::CO_SUSPEND;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_YieldCo(shared_from_this());
#endif
    g_bbt_dbgp_full(("[Coroutine::YieldAndPushGCoQueue] co=" + std::to_string(GetId())).c_str());
    m_context.Yield();
}


CoroutineId Coroutine::GetId() const noexcept
{
    return m_id;
}

CoroutineStatus Coroutine::GetStatus() const noexcept
{
    return m_run_status;
}

void Coroutine::OnException() noexcept
{
    m_yield_disposition = CoroutineYieldDisposition::FINAL;
    m_run_status = CoroutineStatus::CO_FINAL;
    if (auto ev = _AwaitEvent()) {
        ev->UnRegist();
        _SetAwaitEvent(nullptr);
    }
}

void Coroutine::RequestCancel() noexcept
{
    m_cancel_requested.store(true, std::memory_order_release);
    if (auto ev = _AwaitEvent())
        ev->Trigger(EventOpt::TIMEOUT);
}

bool Coroutine::IsCancelRequested() const noexcept
{
    return m_cancel_requested.load(std::memory_order_acquire);
}

std::shared_ptr<CoPollEvent> Coroutine::_AwaitEvent() const
{
    std::lock_guard<std::mutex> lk(m_await_mu);
    return m_await_event;
}

void Coroutine::_SetAwaitEvent(std::shared_ptr<CoPollEvent> ev)
{
    std::lock_guard<std::mutex> lk(m_await_mu);
    m_await_event = std::move(ev);
}

int Coroutine::YieldUntilTimeout(int ms)
{
    if (IsCancelRequested())
        return 1;

    Assert(_AwaitEvent() == nullptr);
    _SetAwaitEvent(CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    }));

    if (_AwaitEvent()->InitFdEvent(-1, EventOpt::TIMEOUT, ms) != 0) {
        _SetAwaitEvent(nullptr);
        return -1;
    }

    return YieldWithCallback([this](){
        return _RegistAwaitEvent();
    });
}

std::shared_ptr<CoPollEvent> Coroutine::RegistCustom(int key)
{
    if (_AwaitEvent() != nullptr)
        return nullptr;

    auto ev = CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });
    _SetAwaitEvent(ev);

    if (ev->InitCustomEvent(key, nullptr) != 0) {
        _SetAwaitEvent(nullptr);
        return nullptr;
    }

    return ev;
}

std::shared_ptr<CoPollEvent> Coroutine::RegistCustom(int key, int timeout_ms)
{
    if (_AwaitEvent() != nullptr)
        return nullptr;

    auto ev = CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });
    _SetAwaitEvent(ev);

    if (ev->InitCustomEvent(key, nullptr) != 0) {
        _SetAwaitEvent(nullptr);
        return nullptr;
    }

    if (ev->InitFdEvent(-1, EventOpt::TIMEOUT, timeout_ms) != 0) {
        _SetAwaitEvent(nullptr);
        return nullptr;
    }

    return ev;
}

int Coroutine::YieldUntilFdReadable(int fd)
{
    if (IsCancelRequested())
        return 1;
    Assert(_AwaitEvent() == nullptr);
    auto ev = CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });
    _SetAwaitEvent(ev);

    if (ev->InitFdEvent(fd, EventOpt::READABLE | EventOpt::FINALIZE, 0) != 0) {
        _SetAwaitEvent(nullptr);
        return -1;
    }

    return YieldWithCallback([this](){
        return _RegistAwaitEvent();
    });
}

int Coroutine::YieldUntilFdReadable(int fd, int timeout_ms)
{
    if (IsCancelRequested())
        return 1;
    Assert(_AwaitEvent() == nullptr);

    auto ev = CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });
    _SetAwaitEvent(ev);

    if (ev->InitFdEvent(fd, EventOpt::READABLE | EventOpt::TIMEOUT | EventOpt::FINALIZE, timeout_ms) != 0) {
        _SetAwaitEvent(nullptr);
        return -1;
    }

    return YieldWithCallback([this](){
        return _RegistAwaitEvent();
    });
}

int Coroutine::YieldUntilFdWriteable(int fd)
{
    if (IsCancelRequested())
        return 1;
    Assert(_AwaitEvent() == nullptr);
    auto ev = CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });
    _SetAwaitEvent(ev);

    if (ev->InitFdEvent(fd, EventOpt::WRITEABLE | EventOpt::FINALIZE, 0) != 0) {
        _SetAwaitEvent(nullptr);
        return -1;
    }

    return YieldWithCallback([this](){
        return _RegistAwaitEvent();
    });
}

int Coroutine::YieldUntilFdWriteable(int fd, int timeout_ms)
{
    if (IsCancelRequested())
        return 1;
    Assert(_AwaitEvent() == nullptr);
    auto ev = CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });
    _SetAwaitEvent(ev);

    if (ev->InitFdEvent(fd, EventOpt::WRITEABLE | EventOpt::TIMEOUT | EventOpt::FINALIZE, timeout_ms) != 0) {
        _SetAwaitEvent(nullptr);
        return -1;
    }

    return YieldWithCallback([this](){
        return _RegistAwaitEvent();
    });
}

int Coroutine::YieldUntilFdEx(int fd, short events, int timeout_ms)
{
    if (IsCancelRequested())
        return 1;
    Assert(_AwaitEvent() == nullptr);
    auto ev = CoPollEvent::Create(GetId(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });
    _SetAwaitEvent(ev);

    /* 绝对不可以反复触发 */
    if (ev->InitFdEvent(fd, events & ~pollevent::EventOpt::PERSIST, timeout_ms) != 0) {
        _SetAwaitEvent(nullptr);
        return -1;
    }

    return YieldWithCallback([this](){
        return _RegistAwaitEvent();
    });
}

bool Coroutine::_RegistAwaitEvent()
{
    auto await_event = _AwaitEvent();
    // RequestCancel 可能发生在入口检查之后、事件挂上之前。
    // 注册前再看一眼：已置位则 Trigger，CommitPark 走 PENDING 立即完成。
    if (await_event != nullptr && IsCancelRequested())
        await_event->Trigger(EventOpt::TIMEOUT);

    if (await_event != nullptr && await_event->Regist() == 0)
        return true;

    _SetAwaitEvent(nullptr);
    m_yield_disposition = CoroutineYieldDisposition::MANUAL;
    return false;
}

CoroutineYieldDisposition Coroutine::CommitYield()
{
    if (m_run_status == CoroutineStatus::CO_FINAL) {
        m_yield_disposition = CoroutineYieldDisposition::FINAL;
        return CoroutineYieldDisposition::FINAL;
    }

    const auto disposition = m_yield_disposition;
    if (disposition == CoroutineYieldDisposition::EVENT_WAIT) {
        auto await_event = _AwaitEvent();
        Assert(await_event != nullptr);
        await_event->CommitPark();
    }

    return disposition;
}


void Coroutine::OnCoPollEvent(int event, int custom_key)
{
    CoroutinePriority priority = CO_PRIORITY_NORMAL;

    // MLFQ: 运行时长超过时间片 → 降级以让出 CPU 给其他协程
    constexpr uint64_t kMlfqTimeSliceUs = 1000; // 1ms
    if (m_last_run_us > kMlfqTimeSliceUs)
    {
        if (m_mlfq_demotions < 3)
            m_mlfq_demotions++;
        priority = CO_PRIORITY_LOW;
    }
    else if (m_mlfq_demotions > 0)
    {
        // 运行时间收敛 → 逐步恢复优先级
        m_mlfq_demotions--;
    }

    auto ev = _AwaitEvent();
    Assert(ev != nullptr);

    m_last_resume_event = event;

    // 先取消事件，然后push到全局队列中
    g_bbt_dbgp_full(("[CoEvent:Trigger] co=" + std::to_string(GetId()) + " trigger_event=" + std::to_string(event) + " id=" + std::to_string(ev->GetId()) + " customkey=" + std::to_string(custom_key)).c_str());
    _SetAwaitEvent(nullptr);
    m_yield_disposition = CoroutineYieldDisposition::MANUAL;

    // 超时任务优先级最高，覆盖 MLFQ 判定
    if (event & EventOpt::TIMEOUT)
        priority = CO_PRIORITY_CRITICAL;

    g_scheduler->OnActiveCoroutine(priority, this);

}

int Coroutine::GetLastResumeEvent() const noexcept
{
    return m_last_resume_event;
}

size_t Coroutine::GetStackSize() const noexcept
{
    return m_context.GetStackSize();
}

}
