#include <atomic>
#include <algorithm>
#include <cstdio>
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

/* #369 单测故障注入标志（定义，声明见 Coroutine.hpp） */
std::atomic_bool                Coroutine::s_test_fail_await_regist{false};

CoroutineId Coroutine::_GenCoroutineId()
{
    static std::atomic_uint64_t _generate_id{BBT_COROUTINE_INVALID_COROUTINE_ID};
    return (++_generate_id);
}

Coroutine::Ptr Coroutine::Create(size_t stack_size, const CoroutineCallback& co_func, bool need_protect, const char* desc)
{
    /* 发布前暂持 unique_ptr：描述赋值若抛出（bad_alloc）也必须销毁半成品，
     * 不能让已借出的栈随裸指针泄漏。 */
    std::unique_ptr<Coroutine> co(new Coroutine(stack_size, co_func, need_protect));
    if (desc != nullptr && desc[0] != '\0')
        co->m_desc = desc;    // #276：bbtco_desc 落库
    return co.release();
}

Coroutine::Coroutine(size_t stack_size, const CoroutineCallback& co_func, bool need_protect):
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
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    /* #404：销毁是所有驱动方式的共同出口。直接 Create/Resume/delete
     * 的终态让出不经过 CommitYield，m_co_map 里可能仍挂着本协程的
     * 借用登记——对象一销毁即成悬空指针，必须在此按 id 幂等清账。
     * 受管路径的配对核销仍由 CommitYield FINAL 完成，本调用只兜底。 */
    g_bbt_dbgmgr->OnEvent_DestroyCo(m_id);
#endif
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_DestoryCoroutine();
#endif
}

void Coroutine::Resume()
{
    Assert(m_run_status == CoroutineStatus::CO_RUNNABLE || m_run_status == CoroutineStatus::CO_SUSPEND);
    m_run_status = CoroutineStatus::CO_RUNNING;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    /* #404：Coroutine 为裸指针借用对象（Create 返回 Ptr、不归 shared_ptr
     * 所有），无 enable_shared_from_this；诊断表只登记借用的 this，
     * 与 DebugMgr 的 Coroutine* 签名及实际所有权一致。 */
    g_bbt_dbgmgr->OnEvent_ResumeCo(this);
#endif
    /* 日志装配不分配：本函数在 worker 原生栈执行，std::to_string 拼接的
     * bad_alloc 会逃出 Resume 直达 Processer 调度循环，不在协程侧任何
     * catch 可达域内。栈缓冲 snprintf 保持同一日志语义、消除逃逸点。 */
    char resume_log[64];
    std::snprintf(resume_log, sizeof(resume_log), "[Coroutine::Resume] co=%llu",
                  static_cast<unsigned long long>(GetId()));
    g_bbt_dbgp_full(resume_log);
    m_context.Resume();
}

void Coroutine::Yield()
{
    Assert(m_run_status == CoroutineStatus::CO_RUNNING);
    m_yield_disposition = CoroutineYieldDisposition::MANUAL;
    m_run_status = CoroutineStatus::CO_SUSPEND;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_YieldCo(this);
#endif
    char yield_log[64];
    std::snprintf(yield_log, sizeof(yield_log), "[Coroutine::Yield] co=%llu",
                  static_cast<unsigned long long>(GetId()));
    g_bbt_dbgp_full(yield_log);
    m_context.Yield();
}

int Coroutine::YieldWithCallback(const CoroutineOnYieldCallback& cb)
{
    /**
     * 确保协程挂起后，才可以触发事件
     */
    Assert(m_run_status == CoroutineStatus::CO_RUNNING);
    /* R2-01：日志装配不分配——snprintf 进栈缓冲，OOM 无处抛出，原 R2-01
     * 故障点（to_string 动态拼接）已整体移除。armed 注入的首个真实分配
     * 因此落在 Context::YieldWithCallback 的 m_onyield_callback 存储上，
     * 由下方统一 catch 回滚。宏关闭时 snprintf 语句仍执行但为零成本诊断，
     * 保持与既有 g_bbt_dbgp_full 调用点一一对应。 */
    char yieldcb_log[80];
    std::snprintf(yieldcb_log, sizeof(yieldcb_log),
                  "[Coroutine::YieldWithCallback] co=%llu",
                  static_cast<unsigned long long>(GetId()));
    g_bbt_dbgp_full(yieldcb_log);
    /* #368-R2/F3：callback 装配链上可分配的失败点为本层入参拷贝与
     * Context::YieldWithCallback 内同一 `m_onyield_callback = cb` 存储赋值。
     * 任一抛出时协程仍在 CO_RUNNING、尚未让出；归还 await 事件（INITED
     * 事件由 shared_ptr 析构回收）后原样上抛。不做「先干跑一遍存储」的
     * 预演：多一次拷贝不证明下一次成功，反而多执行一次 callable 拷贝/析构
     * 副作用；唯一存储点的真实失败由 Context 侧收口（空槽 + NO_CHECK），
     * 下方 catch 统一完成 await/运行态回滚。 */
    m_yield_disposition = _AwaitEvent() == nullptr ?
        CoroutineYieldDisposition::MANUAL : CoroutineYieldDisposition::EVENT_WAIT;
    m_run_status = CoroutineStatus::CO_SUSPEND;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_YieldCo(this);
#endif
    /* YieldWithCallback 内唯一存储点失败（target 非 SBO 复制遇上 OOM）时
     * 协程尚未让出但已标 CO_SUSPEND——按 CHECK_FAILED 相同口径回滚
     * CO_RUNNING 并清 await 后重抛。 */
    int ret = 0;
    try {
        ret = m_context.YieldWithCallback(cb);
    } catch (...) {
        _SetAwaitEvent(nullptr);
        m_yield_disposition = CoroutineYieldDisposition::MANUAL;
        m_run_status = CoroutineStatus::CO_RUNNING;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
        g_bbt_dbgmgr->OnEvent_ResumeCo(this);
#endif
        throw;
    }
    /* #369：挂起回调失败（CHECK_FAILED）时协程被立刻重入、并未真正挂起，
     * 必须把运行态还原为 CO_RUNNING——否则 m_run_status 残留 CO_SUSPEND，
     * 本协程仍在运行却对外谎报挂起，下一次 Yield* 的 CO_RUNNING 断言会
     * 误触发（单测中表现为 SIGABRT）。仅失败路径补偿，正常唤醒路径的
     * 状态推进不变。 */
    if (bbt_unlikely(ret != 0)) {
        m_yield_disposition = CoroutineYieldDisposition::MANUAL;
        m_run_status = CoroutineStatus::CO_RUNNING;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
        /* #404：CHECK_FAILED 重入走 Context::_Resume 直达协程，不经过
         * Coroutine::Resume，m_co_map 不会产生新的 Resume 记账；但协程
         * 恢复运行后下一次 Yield* 仍会走 OnEvent_YieldCo 核销——此处按
         * 「重入即一次运行区间起点」补登记，保持 resume/yield 严格配对。 */
        g_bbt_dbgmgr->OnEvent_ResumeCo(this);
#endif
    }
    return ret;
}

void Coroutine::YieldAndPushGCoQueue()
{
    Assert(m_run_status == CoroutineStatus::CO_RUNNING);
    m_yield_disposition = CoroutineYieldDisposition::READY;
    m_run_status = CoroutineStatus::CO_SUSPEND;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_YieldCo(this);
#endif
    char yieldpush_log[80];
    std::snprintf(yieldpush_log, sizeof(yieldpush_log),
                  "[Coroutine::YieldAndPushGCoQueue] co=%llu",
                  static_cast<unsigned long long>(GetId()));
    g_bbt_dbgp_full(yieldpush_log);
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

void Coroutine::OnException(std::exception_ptr eptr) noexcept
{
    m_exception = std::move(eptr);
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
    /* #347②：取消唤醒使用独立取消位，不再借用超时位——恢复侧可凭
     * 唤醒掩码直接区分「被取消」与「真超时」。 */
    if (auto ev = _AwaitEvent())
        ev->Trigger(POLL_EVENT_CANCELLED);
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

std::exception_ptr Coroutine::GetException() const noexcept
{
    return m_exception;
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

void Coroutine::_TestFailNextAwaitRegist() noexcept
{
    s_test_fail_await_regist.store(true, std::memory_order_release);
}

bool Coroutine::_RegistAwaitEvent()
{
    auto await_event = _AwaitEvent();
    /* #369 单测故障注入：置位时先取消本次 await 事件，让下方 Regist() 走
     * 真实失败分支（CANCELLED 态 CAS 失败），等价于生产上事件在挂起回调
     * 执行前已被反注册的时序；一次性消费，不影响后续等待。 */
    if (await_event != nullptr &&
        s_test_fail_await_regist.exchange(false, std::memory_order_acq_rel))
    {
        await_event->UnRegist();
    }
    // RequestCancel 可能发生在入口检查之后、事件挂上之前。
    // 注册前再看一眼：已置位则以取消位 Trigger，CommitPark 走 PENDING 立即完成。
    if (await_event != nullptr && IsCancelRequested())
        await_event->Trigger(POLL_EVENT_CANCELLED);


    if (await_event != nullptr && await_event->Regist() == 0)
    {
        /* 现场时戳（#276）：parked 起点；唤醒时清零 */
        m_parked_us = bbt::core::clock::gettime_mono<bbt::core::clock::microseconds>();
        return true;
    }

    _SetAwaitEvent(nullptr);
    m_yield_disposition = CoroutineYieldDisposition::MANUAL;
    return false;
}

int Coroutine::GetWaitInfo(CoroutineWaitInfo& out) const noexcept
{
    /* 仅协程自身线程调用安全（同 Processer，无并发写者）；见头文件注释 */
    if (m_await_event == nullptr || m_parked_us == 0)
        return -1;

    out.m_wait_event = m_await_event->GetEvent();
    out.m_fd = (out.m_wait_event & (PollEventType::POLL_EVENT_READABLE | PollEventType::POLL_EVENT_WRITEABLE))
               ? m_await_event->GetFd() : -1;
    out.m_timeout_ms = m_await_event->GetTimeout() > 0 ? m_await_event->GetTimeout() : 0;
    out.m_waited_us = bbt::core::clock::gettime_mono<bbt::core::clock::microseconds>() - m_parked_us;
    return 0;
}

CoroutineYieldDisposition Coroutine::CommitYield()
{
    if (m_run_status == CoroutineStatus::CO_FINAL) {
        m_yield_disposition = CoroutineYieldDisposition::FINAL;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
        /* #404：FINAL 的最后一次让出发生在 Context::_CoroutineMain 尾部，
         * 走 Context::Yield → _Yield 直达调度线程，不经过 Coroutine::Yield
         * ——m_co_map 里本协程的 Resume 记账没有对应核销点。此处是 Processer
         * 在协程终态上下文退出后、delete 之前的唯一收口位置（含正常与异常
         * 两条 FINAL 路径），按「终态让出」核销，维持配对不变式。 */
        g_bbt_dbgmgr->OnEvent_YieldCo(this);
#endif
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
    /* #370：await_event 即将被消费清空，先把本次等待的 fd 与登记时采样的
     * 代际捕获下来——恢复后的 syscall 重试边界据此识别 close+reuse，
     * 覆盖「正常事件已就绪但尚未恢复」的调度间隙（此时 waiter 已被摘除，
     * 仅 epoch 仍能证明 fd 是否已换代）。 */
    m_last_wait_fd = ev->GetFd();
    m_last_wait_epoch = ev->GetWaitEpoch();
    m_parked_us = 0;  // 唤醒即清等待现场（#276）

    // 先取消事件，然后push到全局队列中
    /* 日志装配不分配：本函数经 _Complete 的 callback 在 Processer 原生栈
     * 执行，to_string 拼接的 bad_alloc 会中断唤醒回调（_SetAwaitEvent 与
     * 入队被跳过）致协程悬挂。栈缓冲 snprintf 同型消除该逃逸点。 */
    char trigger_log[160];
    std::snprintf(trigger_log, sizeof(trigger_log),
                  "[CoEvent:Trigger] co=%llu trigger_event=%d id=%llu customkey=%d",
                  static_cast<unsigned long long>(GetId()),
                  static_cast<int>(event),
                  static_cast<unsigned long long>(ev->GetId()),
                  custom_key);
    g_bbt_dbgp_full(trigger_log);
    _SetAwaitEvent(nullptr);
    m_yield_disposition = CoroutineYieldDisposition::MANUAL;

    // 超时任务优先级最高，覆盖 MLFQ 判定；#347② 取消改用独立位后，
    // 取消唤醒必须同样获得 CRITICAL 提升，否则是静默的调度行为回归。
    if ((event & EventOpt::TIMEOUT) || (event & POLL_EVENT_CANCELLED))
        priority = CO_PRIORITY_CRITICAL;

    /* 底层 Event 必须先析构，否则同一 FD 的下一次等待会在 ASIO 重复注册。 */
    g_bbt_poller->FlushDeferredEvents();
    g_scheduler->OnActiveCoroutine(priority, this);

}

int Coroutine::GetLastResumeEvent() const noexcept
{
    return m_last_resume_event;
}

bool Coroutine::FdWaitEpochValid(int fd) const noexcept
{
    /* #370：本次等待以 POLL_EVENT_CLOSED 结束，或 fd 代际在挂起期间被
     * BeginFdClose 推进，都说明 fd 数字可能已复用为新对象——重试 syscall
     * 会作用在错误对象上。等不到的 fd 事件（m_last_wait_fd<0 或 fd 不
     * 匹配）视为无代际信息，放行由原生 EBADF 兜底。 */
    if (m_last_resume_event & POLL_EVENT_CLOSED)
        return false;
    if (m_last_wait_fd < 0 || m_last_wait_fd != fd)
        return true;
    return CoPollEvent::FdEpoch(fd) == m_last_wait_epoch;
}

size_t Coroutine::GetStackSize() const noexcept
{
    return m_context.GetStackSize();
}

}
