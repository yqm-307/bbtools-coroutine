#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/DriverWakeDeferral.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

namespace bbt::coroutine::detail
{

namespace
{

/**
 * @brief 单代 driver callback lease（栈上 RAII，不可移动）。
 *
 * 构造：持 binding 强引用，在 binding 锁内按状态决定是否授予并
 * ++inflight；析构：锁内 --inflight，归零时唤醒解绑等待者。
 *  - 普通 callback（Tick/Wait/Wake）：仅 Bound 授予。
 *  - 终止 Wake（terminal=true）：Bound/Failed/Detaching 且未 detached
 *    时授予——解绑期间的最后一次 Wake 不能被 Detaching 拒绝。
 *
 * TLS 链式 lease 栈：每次成功授予把本对象压入线程局部队首，用于
 * Unregister 检测「当前线程持有该代任一 lease」（含外线程 Wake 与
 * 嵌套调用），不引入额外分配。
 */
class DriverCallbackLease
{
public:
    /* 普通 callback 构造：锁内按状态授予并 ++inflight */
    DriverCallbackLease(const std::shared_ptr<ProcesserDriverBinding>& binding, bool terminal)
        : m_binding(binding)
    {
        auto& b = *m_binding;
        std::lock_guard<std::mutex> lock(b.m_mutex);
        const bool granted = !b.m_detached &&
            (b.m_state == ProcesserDriverState::Bound ||
             (terminal && b.m_state == ProcesserDriverState::Detaching) ||
             (terminal && b.m_state == ProcesserDriverState::Failed));
        if (!granted)
            return;

        ++b.m_inflight;
        m_prev = _TlsHead();
        _TlsHead() = this;
        m_acquired = true;
    }

    /* AdoptTag：inflight 已由解绑 leader 在 Detaching 锁内预留，adopt 只
     * 压入 TLS 栈（供 ThreadHolds 检测本线程持该代 lease），不再 ++；
     * 析构照常 pop 与 --/notify。终止 Wake 必须走此入口，否则嵌套同代
     * Unregister 会绕过 lease 检测等待自身。 */
    struct AdoptReservedTag {};
    DriverCallbackLease(const std::shared_ptr<ProcesserDriverBinding>& binding, AdoptReservedTag)
        : m_binding(binding)
    {
        m_prev = _TlsHead();
        _TlsHead() = this;
        m_acquired = true;
    }

    DriverCallbackLease(const DriverCallbackLease&) = delete;
    DriverCallbackLease& operator=(const DriverCallbackLease&) = delete;

    ~DriverCallbackLease()
    {
        if (!m_acquired)
            return;

        // LIFO：lease 只在同一线程的栈作用域内嵌套，本对象必在队首
        Assert(_TlsHead() == this);
        _TlsHead() = m_prev;

        auto& b = *m_binding;
        {
            std::lock_guard<std::mutex> lock(b.m_mutex);
            --b.m_inflight;
        }
        b.m_cv.notify_all();
    }

    bool                        Acquired() const { return m_acquired; }

    /* 当前线程是否持有该 binding（同代）的任一 lease */
    static bool                 ThreadHolds(const ProcesserDriverBinding* binding) noexcept
    {
        for (auto* p = _TlsHead(); p != nullptr; p = p->m_prev)
            if (p->m_binding.get() == binding)
                return true;
        return false;
    }
private:
    static DriverCallbackLease*& _TlsHead() noexcept
    {
        static thread_local DriverCallbackLease* tl_head = nullptr;
        return tl_head;
    }

    std::shared_ptr<ProcesserDriverBinding> m_binding;
    DriverCallbackLease*                  m_prev{nullptr};
    bool                                  m_acquired{false};
};

/* 轻量 busy 信号的作用域守卫：只覆盖用户协程 Resume 本身，不依赖
 * m_cfg_worker_stall_warn_ms。异常退出也必须复位，否则自动缩容会
 * 把该 worker 永久认为忙。 */
class BusyScope
{
public:
    explicit BusyScope(std::atomic_bool& busy): m_busy(busy)
    {
        m_busy.store(true, std::memory_order_release);
    }
    ~BusyScope()
    {
        m_busy.store(false, std::memory_order_release);
    }
    BusyScope(const BusyScope&) = delete;
    BusyScope& operator=(const BusyScope&) = delete;
private:
    std::atomic_bool& m_busy;
};

} // namespace

Processer::SPtr Processer::Create()
{
    return std::make_shared<Processer>();
}

Processer::SPtr& Processer::_GetLocalProcesserStorage()
{
    // 创建型入口与只读查询必须共享唯一的 TLS 实例。
    static thread_local Processer::SPtr tl_processer = nullptr;
    return tl_processer;
}

Processer::SPtr Processer::TryGetLocalProcesser()
{
    return _GetLocalProcesserStorage();
}

Processer::SPtr Processer::GetLocalProcesser()
{
    auto& tl_processer = _GetLocalProcesserStorage();
    if (tl_processer == nullptr) {
        tl_processer = Create();
#ifdef BBT_COROUTINE_PROFILE
        g_bbt_profiler->OnEvent_StartProcesser(tl_processer);
#endif
    }

    return tl_processer;
}

void Processer::_InstallLocalProcesser(SPtr proc)
{
    /* commit 阶段已完成 Scheduler/Profiler 登记；这里只把新线程的 TLS
     * 指向同一个 P，避免惰性创建第二个未登记的 Processer。 */
    _GetLocalProcesserStorage() = std::move(proc);
}

ProcesserId Processer::_GenProcesserId()
{
    static std::atomic_uint64_t _generate_id{BBT_COROUTINE_INVALID_PROCESSER_ID};
    return (++_generate_id);
}

Processer::Processer():
    m_id(_GenProcesserId())
{
    m_run_status = ProcesserStatus::PROC_SUSPEND;
}

Processer::~Processer()
{
}

ProcesserStatus Processer::GetStatus()
{
    return m_run_status;
}

size_t Processer::GetLoadValue()
{
    return GetExecutableNum();
}

size_t Processer::GetExecutableNum()
{
    return m_coroutine_queue.Size();
}


ProcesserId Processer::GetId()
{
    return m_id;
}

bool Processer::AddCoroutineTask(CoroutinePriority priority, Coroutine::Ptr coroutine)
{
    /* 外部普通投递：入队后需要 _PublishRunnable 唤醒 worker。 */
    return _EnqueueAdmitted(priority, coroutine, /*notify*/true);
}

bool Processer::_EnqueueAdmitted(CoroutinePriority priority, Coroutine::Ptr coroutine, bool notify)
{
    AssertWithInfo(coroutine != nullptr, "coroutine is nullptr!");
    if (!m_admission_open.load(std::memory_order_acquire))
        return false;

    /* admission 与入队的线性化：先登记 in-flight，再复查 admission。
     * retire 先关闭 admission 后，本路径要么在关闭前完成入队（随后由
     * worker drain 收回），要么在复查时拒绝，绝不落入已排空的 retired
     * 本地队列。 */
    m_enqueue_inflight.fetch_add(1, std::memory_order_seq_cst);
    if (!m_admission_open.load(std::memory_order_seq_cst)) {
        _ReleaseEnqueueSlot();
        return false;
    }

    const bool enqueued = m_coroutine_queue.Enqueue(coroutine->m_cfs, coroutine, priority,
        g_bbt_coroutine_config->m_cfg_processer_priority_runtime_budget_us[priority]);
    _ReleaseEnqueueSlot();
    if (!enqueued)
        return false;   // 调用方仍持有协程所有权，可重试/显式拒绝

    /* 锁内只发布 latch/cond：driver Wake 由调用方在退出调用链锁后
     * 经 _NotifyRunnable/_WakeDriver 触发（deferral 生效时只记 pending）。
     * notify=false 用于 worker 自身在持 run_cond 的睡前复查/steal 路径，
     * 避免对同一把 m_run_cond_mutex 自锁。 */
    if (notify)
        _PublishRunnable();
    return true;
}

void Processer::_ReleaseEnqueueSlot()
{
    if (m_enqueue_inflight.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
        !m_admission_open.load(std::memory_order_acquire)) {
        /* 与 _WaitEnqueueQuiescent 的谓词共用 mutex，避免丢唤醒 */
        std::lock_guard<std::mutex> lock(m_enqueue_wait_mutex);
        m_enqueue_wait_cv.notify_all();
    }
}

void Processer::_WaitEnqueueQuiescent()
{
    std::unique_lock<std::mutex> lock(m_enqueue_wait_mutex);
    m_enqueue_wait_cv.wait(lock, [this] {
        return m_enqueue_inflight.load(std::memory_order_seq_cst) == 0;
    });
}

void Processer::_PublishRunnable()
{
    /* 与 wait 的谓词检查/解锁入睡共用锁。不能只改 atomic 后 notify：
     * 通知可能落在谓词检查与真正入睡之间；也不能按旧状态跳过运行中的 P。 */
    {
        std::lock_guard<std::mutex> lock(m_run_cond_mutex);
        m_run_cond_notify.store(false, std::memory_order_release);
    }
    m_run_cond.notify_one();
}

void Processer::_NotifyRunnable()
{
    _PublishRunnable();
    _WakeDriver();
}

void Processer::_WaitForWork()
{
    /* 已确认退役：不再取全局/等待，交给 _Run 尾部排空本地队列。 */
    if (m_retire_requested.load(std::memory_order_acquire))
        return;

    const auto timeout_us = bbt::core::clock::us(
        g_bbt_coroutine_config->m_cfg_processer_proc_interval_us);

    /* lease 在 run_cond 锁外获取，不建立 binding→run_cond 嵌套锁序；
     * 没有 binding 或 lease 被 Failed/Detaching 拒绝时走原 #393 分支，
     * 不加新的本地 approx 门控（避免 size_approx 假阳性忙等）。 */
    auto binding = _GetDriverBinding();
    if (binding != nullptr) {
        DriverCallbackLease lease(binding, /*terminal*/false);
        if (lease.Acquired()) {
            bool should_wait = false;
            {
                std::unique_lock<std::mutex> lock(m_run_cond_mutex);
                /* 发布在加锁前：在此取得实际全局任务；发布在复查后：通知者
                 * 必须等复查线程退出锁后才能置位 latch。driver Wait 只在
                 * 本地/global 复查均无 runnable 且无待处理 latch 时进入。 */
                if (_TryGetCoroutineFromGlobal() == 0 &&
                    GetExecutableNum() <= 0 &&
                    m_run_cond_notify.load(std::memory_order_acquire))
                    should_wait = true;
                else
                    m_run_cond_notify.store(true, std::memory_order_release);
            }
            if (should_wait) {
                /* 锁外执行 driver Wait；之间到达的 Wake 由 driver 持久记忆，
                 * latch 通知照常置位、下一轮复查不会丢。 */
                ProcesserDriverResult ret = ProcesserDriverResult::Ok;
                try
                {
                    ret = binding->m_driver->Wait(timeout_us);
                }
                catch (...)
                {
                    _DriverFail(binding, ProcesserDriverFailure::Wait, std::current_exception());
                    return;
                }
                if (ret == ProcesserDriverResult::Failed)
                    _DriverFail(binding, ProcesserDriverFailure::Wait, nullptr);
            }
            return;
        }
    }

    /* 无 driver（或 Failed/Detaching 拒绝普通 lease）：保留 #393 原路径，
     * 睡前 global/实际本地队列复查 + 锁内 latch 等待。 */
    std::unique_lock<std::mutex> lock(m_run_cond_mutex);
    if (_TryGetCoroutineFromGlobal() == 0 && GetExecutableNum() == 0)
        m_run_cond.wait_for(lock, timeout_us,
                   [this] {
                       return !m_is_running.load(std::memory_order_acquire) ||
                           !m_run_cond_notify.load(std::memory_order_acquire) ||
                           m_retire_requested.load(std::memory_order_acquire);
                   });
    m_run_cond_notify.store(true, std::memory_order_release);
}

void Processer::_Init()
{
    m_run_status.store(ProcesserStatus::PROC_DEFAULT, std::memory_order_release);
    m_is_running.store(true, std::memory_order_release);
    m_run_cond_notify.store(true, std::memory_order_release);
    m_running_coroutine = nullptr;
    m_lifecycle.store(ProcesserLifecycleState::Preparing, std::memory_order_release);
    m_admission_open.store(false, std::memory_order_release);
    m_retire_requested.store(false, std::memory_order_release);
    m_busy.store(false, std::memory_order_release);
    m_enqueue_inflight.store(0, std::memory_order_release);
}

void Processer::_Activate()
{
    /* commit 阶段在 map/lifecycle 锁内调用：先开 admission，再发布 Active。
     * 观察者通过同一把 Scheduler 锁看到两个状态，不会出现“Active 但拒收”。 */
    m_admission_open.store(true, std::memory_order_release);
    m_lifecycle.store(ProcesserLifecycleState::Active, std::memory_order_release);
}

void Processer::_BeginRetire()
{
    /* 调用方持有 Scheduler 的 lifecycle 互斥，且已确认 slot 为空。
     * 顺序：先关 admission（新投递立即拒绝），再发布 Retiring，最后
     * 置 retire 请求；worker 在自身调度栈的安全点确认。 */
    /* 与 enqueue 的登记/复查及 drain 的计数读取组成 SC 全序；仅用
     * acquire/release 允许双方同时看到旧值，造成 drain 结束后仍入队。 */
    m_admission_open.store(false, std::memory_order_seq_cst);
    m_lifecycle.store(ProcesserLifecycleState::Retiring, std::memory_order_release);
    m_retire_requested.store(true, std::memory_order_release);
}

bool Processer::_IsActive() const noexcept
{
    return m_lifecycle.load(std::memory_order_acquire) == ProcesserLifecycleState::Active;
}

void Processer::_MarkJoined()
{
    m_lifecycle.store(ProcesserLifecycleState::Joined, std::memory_order_release);
}

void Processer::Start(bool background_thread)
{
    _Init();
    if (background_thread)
    {
        auto t = new std::thread([this](){_Run();});
        t->detach();
        return;
    }

    _Run();
}

void Processer::_Run()
{
    /**
     * Processer主循环逻辑
     *
     * 1. 尝试从全局队列或者本地队列取协程对象
     *      - 若取不到，就挂起Processer线程
     *      - 若取得到，本地线程处理协程执行
     *
     * 2. 处理完本地的协程后，尝试取窃取其他P的协程
     *      - 如果窃取不到就挂起当前线程。
     *      - 如果窃取到，就执行窃取到的协程
     *
     * 3. 重复1、2步骤，进程寿命内不设退出条件（无业务停机入口）
     *
     * XXX 这里也许可以优化的点：
     *      - 是否在空闲的时候降低调度频率？
     */
    // 每轮至多 64 次 Resume，给 driver/global/退役检查确定的调度机会。
    while (m_is_running.load(std::memory_order_acquire))
    {
        /* 退役只在完整 disposition 之后、新一轮取工作之前确认：
         * 已提交的 Resume 不会被中断，parked 协程栈也不会被展开。 */
        if (m_retire_requested.load(std::memory_order_acquire))
            break;

        m_run_status = ProcesserStatus::PROC_RUNNING;

        /* driver Tick(before)：每轮调度前至多一次；worker 线程、非业务
         * 协程上下文（m_running_coroutine 为 nullptr）。 */
        _DriverOnTick(ProcesserDriverPhase::BeforeSchedule);

        // 如果本地没有了，尝试去全局取
        if (GetExecutableNum() <= 0)
            _TryGetCoroutineFromGlobal();

        bool any_dequeued = false;
        bool retire_now = false;
        for (unsigned turn = 0; turn < 64; ++turn)
        {
            if (m_retire_requested.load(std::memory_order_acquire)) {
                retire_now = true;
                break;
            }
            auto* selected = m_coroutine_queue.Pick();
            if (selected == nullptr)
                break;
            m_running_coroutine = selected->Task();
            any_dequeued = true;

            AssertWithInfo(m_running_coroutine->GetStatus() != CO_RUNNING && m_running_coroutine->GetStatus() != CO_FINAL, "bad coroutine status!");

            // 执行前设置当前协程缓存（预算单位为微秒，计时必须显式 us）
            m_running_coroutine_begin.exchange(bbt::core::clock::gettime_mono<bbt::core::clock::us>());

            /* worker 无进展快照（#277/#367）：每 Resume 刷新一次。spin/parked
             * 都只执行一次 Resume——区分"执行停顿"靠 executing 标志：
             * 调度线程读到 true 且 begin_ts 超阈值 = 用户代码占死 worker；
             * 读到 false = 协程已让出，不算停顿。整组字段在
             * m_exec_mutex 临界区内一次提交，读者同锁整组复制，
             * 一致性由互斥保证（诊断路径，允许极短锁占用）。 */
            if (g_bbt_coroutine_config->m_cfg_worker_stall_warn_ms > 0)
            {
                std::lock_guard<std::mutex> lock(m_exec_mutex);
                m_exec_co_id = m_running_coroutine->GetId();
                m_exec_begin_us = m_running_coroutine_begin.load(std::memory_order_relaxed);
                m_exec_backlog = GetExecutableNum();
                const auto& d = m_running_coroutine->GetDescription();
                size_t n = d.size() < sizeof(m_exec_desc) - 1 ? d.size() : sizeof(m_exec_desc) - 1;
                memcpy(m_exec_desc, d.data(), n);
                m_exec_desc[n] = '\0';
                m_executing = true;
            }
#ifdef BBT_COROUTINE_PROFILE
            m_co_swap_times++;
#endif
            /* busy 是自动策略的独立信号，始终维护；stall 诊断字段仍
             * 只由 m_cfg_worker_stall_warn_ms 控制。 */
            {
                BusyScope busy_scope(m_busy);
                m_running_coroutine->Resume();
            }
            if (g_bbt_coroutine_config->m_cfg_worker_stall_warn_ms > 0)
            {
                std::lock_guard<std::mutex> lock(m_exec_mutex);
                m_executing = false;
            }
            // MLFQ: 记录运行时长（微秒），供后续降级判断
            m_running_coroutine->SetLastRunTimeUs(
                bbt::core::clock::gettime_mono<bbt::core::clock::us>() - m_running_coroutine_begin.load());

            // elapsed monotonic us，不是线程 CPU 时间。发布 park 前先结清账目。
            m_coroutine_queue.Account(m_running_coroutine->m_cfs,
                m_running_coroutine->GetLastRunTimeUs(),
                m_running_coroutine->m_yield_disposition == CoroutineYieldDisposition::EVENT_WAIT);

            /* 先移交再发布：CommitYield（EVENT_WAIT 的 CommitPark）与
             * READY 入队都可能让该协程被其它 worker
             * 立即恢复销毁；此后 Wake 回调可重入 GetLocalCoroutineId，
             * m_running_coroutine 必须先清空，绝不向回调暴露已移交对象。 */
            Coroutine::Ptr yielded = m_running_coroutine;
            m_running_coroutine = nullptr;
            const auto disposition = yielded->CommitYield();
            if (disposition == CoroutineYieldDisposition::READY) {
                // READY 保留既有 NORMAL 策略；退役门禁拒收时可靠归还 global。
                if (!_EnqueueAdmitted(CO_PRIORITY_NORMAL, yielded, /*notify*/false)) {
                    while (!g_scheduler->_EnqueueGlobal(CO_PRIORITY_NORMAL, yielded))
                        std::this_thread::yield();
                    g_scheduler->_WakeActiveWorkers();
                }
                yielded = nullptr;  // 发布后绝不再读 Coroutine
            }
            else if (disposition == CoroutineYieldDisposition::FINAL) {
                delete yielded;
            }
        }

        if (retire_now)
            break;

        // 检测 size_approx 假阳性：认为有任务但取不到任何协程
        if (!any_dequeued && GetExecutableNum() > 0) {
#ifdef BBT_COROUTINE_PROFILE
            m_stall_loop_count++;
#endif
        }

        // 每 CHECK_INTERVAL 轮无条件检查全局队列
        // 持续 yield 仍需把外部唤醒搬入本地集合
        if (m_retire_requested.load(std::memory_order_acquire))
            break;

        if (++m_global_check_counter >= kGlobalCheckInterval) {
            m_global_check_counter = 0;
            _TryGetCoroutineFromGlobal();
        }

        /* driver Tick(after)：调度、预算与 global 检查之后、公平轮次
         * continue 之前——本轮 dequeued 且仍有本地任务的快进路径也必须
         * 经过 after，保证 before/after 成对覆盖每个调度轮次。 */
        _DriverOnTick(ProcesserDriverPhase::AfterSchedule);

        /* 本轮确实出队且仍有本地任务：继续有界轮次，不把 runnable 留给轮询。 */
        if (any_dequeued && GetExecutableNum() > 0)
            continue;

        if (m_retire_requested.load(std::memory_order_acquire))
            break;

        m_run_status = ProcesserStatus::PROC_SUSPEND;
        auto steal_num = g_scheduler->TryWorkSteal(shared_from_this());
#ifdef BBT_COROUTINE_PROFILE
        m_steal_succ_times += steal_num;
#endif

        if (steal_num <= 0)
        {
#ifdef BBT_COROUTINE_PROFILE
            auto begin = bbt::core::clock::now<bbt::core::clock::microseconds>();
#endif
            _WaitForWork();
#ifdef BBT_COROUTINE_PROFILE
            m_suspend_cost_times.fetch_add(
                std::chrono::duration_cast<bbt::core::clock::us>(
                    bbt::core::clock::now<bbt::core::clock::microseconds>() - begin).count(),
                std::memory_order_relaxed);
#endif
        }
    }

    if (m_retire_requested.load(std::memory_order_acquire)) {
        /* 退役者自身的收尾顺序：排空本地 runnable -> 唤醒剩余 active ->
         * 发布 Exited -> 线程函数返回。PROC_EXIT 只表示调度函数退出，
         * 不证明 TLS 析构或 reaper join 完成。 */
        _DrainLocalToGlobal();
        g_scheduler->_WakeActiveWorkers();
        m_lifecycle.store(ProcesserLifecycleState::Exited, std::memory_order_release);
    }

    m_run_status = ProcesserStatus::PROC_EXIT;
}


size_t Processer::_TryGetCoroutineFromGlobal()
{
    /* retired worker 不再从全局取任务；进入 drain 前的最后一道门。 */
    if (!m_admission_open.load(std::memory_order_acquire))
        return 0;

    /* 希望获取的总数 */
    auto expected_size = g_bbt_coroutine_config->m_cfg_processer_get_co_from_g_count;
    /* 已经处理的数量 */
    size_t already_count = 0;

    // 跨轮保留游标：即使配置每次仅取一个，也不能让 CRITICAL 持续挡住其它级别。
    unsigned empty_levels = 0;
    while (already_count < expected_size && empty_levels < CO_PRIORITY_COUNT) {
        const auto priority = static_cast<CoroutinePriority>(m_global_pull_priority);
        m_global_pull_priority = (m_global_pull_priority + CO_PRIORITY_COUNT - 1) % CO_PRIORITY_COUNT;
        const auto pulled = g_scheduler->GetCoroutineFromGlobal(priority, 1, *this);
        already_count += pulled;
        empty_levels = pulled == 0 ? empty_levels + 1 : 0;
    }

    return already_count;
}

Coroutine::Ptr Processer::GetCurrentCoroutine()
{
    return m_running_coroutine;
}

uint64_t Processer::GetContextSwapTimes()
{
#ifdef BBT_COROUTINE_PROFILE
    return m_co_swap_times.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

uint64_t Processer::GetSuspendCostTime()
{
#ifdef BBT_COROUTINE_PROFILE
    return m_suspend_cost_times.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

uint64_t Processer::GetStealSuccTimes()
{
#ifdef BBT_COROUTINE_PROFILE
    return m_steal_succ_times.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

uint64_t Processer::GetStealCount()
{
#ifdef BBT_COROUTINE_PROFILE
    return m_steal_count.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

uint64_t Processer::GetStallLoopCount()
{
#ifdef BBT_COROUTINE_PROFILE
    return m_stall_loop_count.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

size_t Processer::Steal(Processer::SPtr thief)
{
    /* victim 已退役则其本地队列正被 drain；thief 正在退役则不得再收新任务。
     * 两者都通过 admission/lifecycle gate 判断，不依赖近似队长。 */
    if (thief == nullptr)
        return 0;
    if (!m_admission_open.load(std::memory_order_acquire) ||
        !_IsActive())
        return 0;
    if (!thief->_IsActive())
        return 0;

    Coroutine::Ptr item = nullptr;
    size_t steal_num = 0;
    size_t expect_size = 0;

    /* 如果没有任务就返回 */
    size_t size = GetExecutableNum();
    if (size <= 0)
        return steal_num;

    /* 可变范围需要空闲 worker 及时分摊已有积压：连续短 Resume 会不断重置
     * 起点，沿用停顿阈值会让新增容量始终空闲。固定规模保留原策略；bounds
     * 在 Start 发布初始化前固定，运行期只读。双方 admission 门禁仍不变。 */
    if (g_scheduler->m_min_num == g_scheduler->m_max_num) {
        const uint64_t prev_run = m_running_coroutine_begin.load();
        const auto already_run_time_us =
            bbt::core::clock::gettime_mono<bbt::core::clock::us>() - prev_run;
        if (already_run_time_us <
            static_cast<uint64_t>(g_bbt_coroutine_config->m_cfg_processer_worksteal_timeout_ms) * 1000)
            return steal_num;
    }

    /* 计算下需要偷多少个 */
    expect_size = g_bbt_coroutine_config->m_cfg_processer_steal_once_min_task_num > size / 2 ? g_bbt_coroutine_config->m_cfg_processer_steal_once_min_task_num : size / 2;

    while (steal_num < expect_size)
    {
        auto* entity = m_coroutine_queue.Take();
        if (entity == nullptr)
            break;
        item = entity->Task();
        const auto priority = static_cast<CoroutinePriority>(entity->Priority());
        // Take 已释放 victim 锁；目标接纳失败仍由当前线程保有 item。
        if (!thief->_EnqueueAdmitted(priority, item, /*notify*/false)) {
            while (!g_scheduler->_EnqueueGlobal(priority, item))
                std::this_thread::yield();
            item = nullptr;
            break;
        }
        item = nullptr;
        ++steal_num;
    }

#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_StealSucc(steal_num);
    m_steal_count += steal_num;
#endif

    return steal_num;
}

void Processer::_DrainLocalToGlobal()
{
    /* 关闭 admission 后先等所有在途 enqueue 收敛，再实际 dequeue；此后
     * 不可能再有人写入本地队列。近似队长不作排空证明。 */
    _WaitEnqueueQuiescent();

    while (auto* entity = m_coroutine_queue.Take())
    {
        auto* item = entity->Task();
        const auto priority = static_cast<CoroutinePriority>(entity->Priority());
        while (!g_scheduler->_EnqueueGlobal(priority, item))
            std::this_thread::yield();
        // item 及 entity 均已发布，不再访问。
    }
}

bool Processer::_HasDriverOwnership()
{
    /* Bound/Failed/Detaching 任一存在即占 slot；只有同代解绑真正清槽后
     * 才回到可退役候选集合。 */
    std::lock_guard<std::mutex> lock(m_driver_slot_mutex);
    return m_driver_binding != nullptr;
}

std::shared_ptr<ProcesserDriverBinding> Processer::_GetDriverBinding()
{
    std::lock_guard<std::mutex> lock(m_driver_slot_mutex);
    return m_driver_binding;
}

void Processer::_DriverOnTick(ProcesserDriverPhase phase)
{
    auto binding = _GetDriverBinding();
    if (binding == nullptr)
        return;

    DriverCallbackLease lease(binding, /*terminal*/false);
    if (!lease.Acquired())
        return;

    ProcesserDriverResult ret = ProcesserDriverResult::Ok;
    try
    {
        ret = binding->m_driver->OnTick(phase);
    }
    catch (...)
    {
        _DriverFail(binding, ProcesserDriverFailure::Tick, std::current_exception());
        return;
    }
    /* 明确返回 Failed 同样记录来源；exception 允许为空 */
    if (ret == ProcesserDriverResult::Failed)
        _DriverFail(binding, ProcesserDriverFailure::Tick, nullptr);
}

void Processer::_DriverFail(const std::shared_ptr<ProcesserDriverBinding>& binding,
                            ProcesserDriverFailure source,
                            std::exception_ptr err)
{
    std::lock_guard<std::mutex> lock(binding->m_mutex);
    /* Detaching 期间到达的故障照常记录，但不把状态回退为 Failed——
     * 解绑已在推进，归属不变。 */
    if (binding->m_state == ProcesserDriverState::Detaching) {
        if (binding->m_failure == ProcesserDriverFailure::None) {
            binding->m_failure = source;
            binding->m_exception = err;
        }
        return;
    }
    if (binding->m_state != ProcesserDriverState::Bound)
        return;

    binding->m_state = ProcesserDriverState::Failed;
    binding->m_failure = source;
    binding->m_exception = err;
}

void Processer::_WakeDriver()
{
    /* deferral 生效（仍在同步原语外层锁内）：只登记 pending，由最外层
     * guard 退出后统一广播；任务/latch 已先行发布，不靠定时醒兜底。 */
    if (DriverWakeDeferral::Defer())
        return;

    auto binding = _GetDriverBinding();
    if (binding == nullptr)
        return;

    DriverCallbackLease lease(binding, /*terminal*/false);
    if (!lease.Acquired())
        return;

    binding->m_driver->Wake();
}

ProcesserDriverStatus Processer::_BindDriver(
    const std::shared_ptr<IProcesserDriver>& driver,
    ProcesserDriverToken& out_token)
{
    /* 调用方（Scheduler::RegisterProcesserDriver）持有 lifecycle 互斥，与
     * 退役提交共用同一线性化点：绑定先到则该 P 被 pin，退役跳过；退役先到
     * 则该 P 已非 Active/已移出 active，返回 InvalidTarget 且 out_token 不变。 */
    if (m_lifecycle.load(std::memory_order_acquire) != ProcesserLifecycleState::Active)
        return ProcesserDriverStatus::InvalidTarget;

    {
        std::lock_guard<std::mutex> lock(m_driver_slot_mutex);
        /* 单槽：Bound/Failed/Detaching 都占归属，detached 后才让位 */
        if (m_driver_binding != nullptr)
            return ProcesserDriverStatus::Occupied;

        if (m_driver_next_generation == UINT64_MAX)
            return ProcesserDriverStatus::InvalidDriver; // 代次溢出拒绝，不重用
        const uint64_t gen = ++m_driver_next_generation;

        auto binding = std::make_shared<ProcesserDriverBinding>();
        binding->m_processer_id = m_id;
        binding->m_generation = gen;
        binding->m_driver = driver;
        m_driver_binding = binding;

        out_token.m_processer_id = m_id;
        out_token.m_generation = gen;
    }

    /* 动态绑定打断旧 CV/Wait 的发布与 driver Wake 由调用方在退出
     * lifecycle/slot 锁后执行（_PublishRunnable/_WakeDriver）。 */
    return ProcesserDriverStatus::Ok;
}

ProcesserDriverStatus Processer::_QueryDriver(ProcesserDriverSnapshot& out_snapshot)
{
    /* 先在 binding 锁内拷局部快照，全部锁外再写给调用方——out 上次的
     * exception_ptr 赋值析构可能释放捕获了 driver/资源的对象，不得在
     * binding 锁内触发外部析构。 */
    ProcesserDriverSnapshot tmp;
    auto binding = _GetDriverBinding();
    if (binding == nullptr) {
        /* 成功查询空槽：Unbound 快照含本 P 身份、gen=0 */
        tmp.m_state = ProcesserDriverState::Unbound;
        tmp.m_token.m_processer_id = m_id;
    } else {
        std::lock_guard<std::mutex> lock(binding->m_mutex);
        tmp.m_state = binding->m_state;
        tmp.m_token.m_processer_id = binding->m_processer_id;
        tmp.m_token.m_generation = binding->m_generation;
        tmp.m_failure = binding->m_failure;
        tmp.m_exception = binding->m_exception;
    }
    out_snapshot = std::move(tmp);   // 锁外写调用方对象（旧值在此析构）
    return ProcesserDriverStatus::Ok;
}

ProcesserDriverStatus Processer::_UnregisterDriver(uint64_t generation)
{
    std::shared_ptr<ProcesserDriverBinding> binding;
    {
        std::lock_guard<std::mutex> lock(m_driver_slot_mutex);
        binding = m_driver_binding;
    }

    /* 精确同代校验：旧 token、伪造 token、并发重绑后的前代 token 一律
     * StaleToken，不能作为任何一代的解绑收据。 */
    if (binding == nullptr || binding->m_generation != generation)
        return ProcesserDriverStatus::StaleToken;

    /* 任何副作用前的自解绑检查：
     *  - 当前线程仍在 runtime 通知临界区（deferral depth>0）时终止 Wake
     *    会穿透 guard 在 runtime 锁内执行，拒绝；
     *  - 当前线程持有该代任一 lease（含外线程 Wake、嵌套 callback）时
     *    同步等待会等自己，拒绝。 */
    if (DriverWakeDeferral::IsActive() ||
        DriverCallbackLease::ThreadHolds(binding.get()))
        return ProcesserDriverStatus::SelfDetach;

    /* leader/joiner：首个到达的控制线程在锁内完成 Detaching 并预留
     * 终止 lease；后到同代线程只等 detached（槽确已解除），不操作新槽。 */
    bool leader = false;
    {
        std::lock_guard<std::mutex> lock(binding->m_mutex);
        if (!binding->m_terminal_wake_reserved) {
            binding->m_state = ProcesserDriverState::Detaching;
            binding->m_terminal_wake_reserved = true;
            ++binding->m_inflight;
            leader = true;
        }
    }

    if (!leader) {
        /* joiner：等待 leader 完成整代解除。m_detached 只在槽已按
         * binding 身份清走后置位，返回 Ok 即该代归属已结束。 */
        std::unique_lock<std::mutex> lock(binding->m_mutex);
        binding->m_cv.wait(lock, [&] { return binding->m_detached; });
        return ProcesserDriverStatus::Ok;
    }

    /* 终止 Wake 在全部锁外执行：adopt 预留 lease 压入 TLS 栈，使 Wake
     * 回调内的嵌套同代 Unregister 被 SelfDetach 拒绝而非等待自身；
     * 它让已获取 lease 但尚未真正 Wait 的 driver 也能退出。 */
    {
        DriverCallbackLease terminal_lease(binding, DriverCallbackLease::AdoptReservedTag{});
        binding->m_driver->Wake();
    }   // 析构：TLS pop + --inflight + notify

    /* 等该代全部 lease（普通 Tick/Wait/Wake 与并发外调）自然归零 */
    {
        std::unique_lock<std::mutex> lock(binding->m_mutex);
        binding->m_cv.wait(lock, [&] { return binding->m_inflight == 0; });
    }

    /* 以 binding 身份清槽：只解除同代归属，不触重绑后的新槽。
     * 最后一个 driver 强引用不得在任何锁内析构。 */
    {
        std::shared_ptr<ProcesserDriverBinding> released;
        {
            std::lock_guard<std::mutex> lock(m_driver_slot_mutex);
            if (m_driver_binding == binding) {
                released = std::move(m_driver_binding);
                m_driver_binding = nullptr;
            }
        }
        released.reset();   // 锁外释放（可能是最后一个 driver 强引用）
    }

    {
        std::lock_guard<std::mutex> lock(binding->m_mutex);
        binding->m_detached = true;
    }
    binding->m_cv.notify_all();

    /* 动态解绑打断旧 CV/Wait：worker 回到无 driver 等待路径 */
    _PublishRunnable();
    return ProcesserDriverStatus::Ok;
}

} // namespace bbt::coroutine::detail
