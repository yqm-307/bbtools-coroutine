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
    size_t total_size = 0;
    for (auto&& it : m_coroutine_queue)
    {
        total_size += it.size_approx();
    }

    return total_size;
}


ProcesserId Processer::GetId()
{
    return m_id;
}

void Processer::AddCoroutineTask(CoroutinePriority priority, Coroutine::Ptr coroutine)
{
    AssertWithInfo(coroutine != nullptr, "coroutine is nullptr!");
    AssertWithInfo(m_coroutine_queue[priority].enqueue(coroutine), "oom!");

    /* 锁内只发布 latch/cond：driver Wake 由调用方在退出调用链锁后
     * 经 _NotifyRunnable/_WakeDriver 触发（deferral 生效时只记 pending）。 */
    _PublishRunnable();
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
     * 睡前 global 复查 + 锁内 latch 等待，不引入新的本地队列门控。 */
    std::unique_lock<std::mutex> lock(m_run_cond_mutex);
    if (_TryGetCoroutineFromGlobal() == 0)
        m_run_cond.wait_for(lock, timeout_us,
                   [this] {
                       return !m_is_running.load(std::memory_order_acquire) ||
                           !m_run_cond_notify.load(std::memory_order_acquire);
                   });
    m_run_cond_notify.store(true, std::memory_order_release);
}

void Processer::_Init()
{
    m_run_status.store(ProcesserStatus::PROC_DEFAULT, std::memory_order_release);
    m_is_running.store(true, std::memory_order_release);
    m_run_cond_notify.store(true, std::memory_order_release);
    m_running_coroutine = nullptr;
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
    /* 优先级运行时间预算（单位：微秒）来自 GlobalConfig，启动前可配置；
     * 初始预算与每次公平轮次重置均从配置拷贝。 */
    auto priority_runtime_budget_us =
        g_bbt_coroutine_config->m_cfg_processer_priority_runtime_budget_us;

    auto reset_priority_budget_if_needed = [&]() {
        bool has_runnable_queue = false;
        bool has_runnable_queue_with_budget = false;
        for (auto&& p : {CO_PRIORITY_CRITICAL,
                         CO_PRIORITY_HIGH,
                         CO_PRIORITY_NORMAL,
                         CO_PRIORITY_LOW})
        {
            if (m_coroutine_queue[p].size_approx() == 0)
                continue;

            has_runnable_queue = true;
            if (priority_runtime_budget_us[p] > 0)
                has_runnable_queue_with_budget = true;
        }

        // 空队列不参与预算耗尽判定；仅当所有非空队列都耗尽预算
        // （或当前没有本地 runnable 队列）时开启下一公平轮次。
        if (!has_runnable_queue || !has_runnable_queue_with_budget)
            priority_runtime_budget_us =
                g_bbt_coroutine_config->m_cfg_processer_priority_runtime_budget_us;
    };

    while (m_is_running.load(std::memory_order_acquire))
    {
        m_run_status = ProcesserStatus::PROC_RUNNING;

        /* driver Tick(before)：每轮调度前至多一次；worker 线程、非业务
         * 协程上下文（m_running_coroutine 为 nullptr）。 */
        _DriverOnTick(ProcesserDriverPhase::BeforeSchedule);

        // 如果本地没有了，尝试去全局取
        if (GetExecutableNum() <= 0)
            _TryGetCoroutineFromGlobal();

        // 对各个优先级任务进行执行，按运行时间预算（微秒）加权轮转
        bool any_dequeued = false;
        for (auto&& p : {CO_PRIORITY_CRITICAL, CO_PRIORITY_HIGH, CO_PRIORITY_NORMAL, CO_PRIORITY_LOW})
        {
            while (priority_runtime_budget_us[p] > 0)
            {
                /* 如果取不到或者取到空的，就退出循环 */
                if (!m_coroutine_queue[p].try_dequeue(m_running_coroutine) || m_running_coroutine == nullptr)
                {
                    /* 本地该优先级队列已空但预算未耗尽：
                     * 按优先级从全局队列补充任务，避免 CRITICAL 全局积压
                     * 使 NORMAL 等低优先级协程长期无法被取回本地执行。
                     * 一次只搬 1 个（最小必要粒度），避免每轮按 16 个批量搬运
                     * 放大全局队列竞争；单轮搬运总量仍由预算循环门控 */
                    if (g_scheduler->GetCoroutineFromGlobal(p, m_coroutine_queue[p], 1) <= 0)
                        break;
                    continue;
                }
                any_dequeued = true;

                AssertWithInfo(m_running_coroutine->GetStatus() != CO_RUNNING && m_running_coroutine->GetStatus() != CO_FINAL, "bad coroutine status!");

                // 执行前设置当前协程缓存（预算单位为微秒，计时必须显式 us）
                m_running_coroutine_begin.exchange(bbt::core::clock::gettime_mono<bbt::core::clock::us>());

                /* worker 无进展快照（#277）：每 Resume 刷新一次。spin/parked
                 * 都只执行一次 Resume——区分"执行停顿"靠 executing 标志：
                 * 调度线程读到 true 且 begin_ts 超阈值 = 用户代码占死 worker；
                 * 读到 false = 协程已让出，不算停顿。 */
                if (g_bbt_coroutine_config->m_cfg_worker_stall_warn_ms > 0)
                {
                    m_exec_seq.store(m_exec_seq.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); // 奇=写中
                    m_exec_co_id = m_running_coroutine->GetId();
                    m_exec_begin_us = m_running_coroutine_begin.load(std::memory_order_relaxed);
                    m_exec_backlog = GetExecutableNum();
                    const auto& d = m_running_coroutine->GetDescription();
                    size_t n = d.size() < sizeof(m_exec_desc) - 1 ? d.size() : sizeof(m_exec_desc) - 1;
                    memcpy(m_exec_desc, d.data(), n);
                    m_exec_desc[n] = '\0';
                    m_exec_seq.store(m_exec_seq.load(std::memory_order_relaxed) + 1, std::memory_order_release); // 偶=稳定
                    m_executing.store(true, std::memory_order_release);
                }
#ifdef BBT_COROUTINE_PROFILE
                m_co_swap_times++;
#endif
                m_running_coroutine->Resume();
                if (g_bbt_coroutine_config->m_cfg_worker_stall_warn_ms > 0)
                    m_executing.store(false, std::memory_order_release);
                // MLFQ: 记录运行时长（微秒），供后续降级判断
                m_running_coroutine->SetLastRunTimeUs(
                    bbt::core::clock::gettime_mono<bbt::core::clock::us>() - m_running_coroutine_begin.load());

                // 按本次实际运行时长扣减优先级预算，单次至少扣 1 微秒
                const auto charged_us = std::max<uint64_t>(
                    1, m_running_coroutine->GetLastRunTimeUs());
                priority_runtime_budget_us[p] =
                    charged_us >= priority_runtime_budget_us[p]
                        ? 0
                        : priority_runtime_budget_us[p] - charged_us;

                /* 先移交再发布：CommitYield（EVENT_WAIT 的 CommitPark）与
                 * READY 的 OnActiveCoroutine 都可能让该协程被其它 worker
                 * 立即恢复销毁；此后 Wake 回调可重入 GetLocalCoroutineId，
                 * m_running_coroutine 必须先清空，绝不向回调暴露已移交对象。 */
                Coroutine::Ptr yielded = m_running_coroutine;
                m_running_coroutine = nullptr;
                const auto disposition = yielded->CommitYield();
                if (disposition == CoroutineYieldDisposition::READY) {
                    g_scheduler->OnActiveCoroutine(CO_PRIORITY_NORMAL, yielded);
                    yielded = nullptr;  // 已移交全局队列，不再读
                }
                else if (disposition == CoroutineYieldDisposition::FINAL) {
                    delete yielded;
                }
            }
        }

        // 检测 size_approx 假阳性：认为有任务但取不到任何协程
        if (!any_dequeued && GetExecutableNum() > 0) {
#ifdef BBT_COROUTINE_PROFILE
            m_stall_loop_count++;
#endif
        }

        // work-conserving：只按非空队列判断预算耗尽，避免空队列的
        // 未用预算阻止公平轮次重置；不提前 continue，保留空转休眠路径。
        reset_priority_budget_if_needed();

        // 每 CHECK_INTERVAL 轮无条件检查全局队列
        // spin worker 会让本地队列 size_approx 始终非零
        if (++m_global_check_counter >= kGlobalCheckInterval) {
            m_global_check_counter = 0;
            _TryGetCoroutineFromGlobal();
        }

        /* driver Tick(after)：调度、预算与 global 检查之后、公平轮次
         * continue 之前——本轮 dequeued 且仍有本地任务的快进路径也必须
         * 经过 after，保证 before/after 成对覆盖每个调度轮次。 */
        _DriverOnTick(ProcesserDriverPhase::AfterSchedule);

        /* 本轮确实出队且仍有本地任务：预算已重置，继续公平轮次，不把
         * 已搬入本地的 runnable 留给轮询。any_dequeued 门控保证
         * size_approx 假阳性至多多转一轮，不形成无任务忙等。 */
        if (any_dequeued && GetExecutableNum() > 0)
            continue;

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
            m_suspend_cost_times += std::chrono::duration_cast<decltype(m_suspend_cost_times)>(bbt::core::clock::now<bbt::core::clock::microseconds>() - begin);
#endif
        }
    }

    m_run_status = ProcesserStatus::PROC_EXIT;
}


size_t Processer::_TryGetCoroutineFromGlobal()
{
    /* 希望获取的总数 */ 
    auto expected_size = g_bbt_coroutine_config->m_cfg_processer_get_co_from_g_count;
    /* 已经处理的数量 */
    size_t already_count = 0;

    /* 每个队列都尝试去全局队列中取一下 */
    for (auto && priority : { CO_PRIORITY_CRITICAL, CO_PRIORITY_HIGH, CO_PRIORITY_NORMAL, CO_PRIORITY_LOW})
    {
        already_count += g_scheduler->GetCoroutineFromGlobal(priority, m_coroutine_queue[priority], expected_size - already_count);
        if (already_count >= expected_size)
            break;
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
    return m_co_swap_times;
#else
    return 0;
#endif
}

uint64_t Processer::GetSuspendCostTime()
{
#ifdef BBT_COROUTINE_PROFILE
    return m_suspend_cost_times.count();
#else
    return 0;
#endif
}

uint64_t Processer::GetStealSuccTimes()
{
#ifdef BBT_COROUTINE_PROFILE
    return m_steal_succ_times;
#else
    return 0;
#endif
}

uint64_t Processer::GetStealCount()
{
#ifdef BBT_COROUTINE_PROFILE
    return m_steal_count;
#else
    return 0;
#endif
}

uint64_t Processer::GetStallLoopCount()
{
#ifdef BBT_COROUTINE_PROFILE
    return m_stall_loop_count;
#else
    return 0;
#endif
}

size_t Processer::Steal(Processer::SPtr thief)
{
    Coroutine::Ptr item = nullptr;
    size_t steal_num = 0;
    size_t expect_size = 0;

    /* 如果没有任务就返回 */
    size_t size = GetExecutableNum();
    if (size <= 0)
        return steal_num;
    
    /* 运行时间过久的才需要偷。
     * m_running_coroutine_begin 与预算同为微秒（gettime_mono<us>），
     * elapsed 必须用 us 计时；与 ms 配置比较前先换算为 us，避免 ms/us 混算 */
    uint64_t prev_run = m_running_coroutine_begin.load();
    auto already_run_time_us =
        bbt::core::clock::gettime_mono<bbt::core::clock::us>() - prev_run;
    if (already_run_time_us <
        static_cast<uint64_t>(g_bbt_coroutine_config->m_cfg_processer_worksteal_timeout_ms) * 1000) {
        return steal_num;
    }

    /* 计算下需要偷多少个 */
    expect_size = g_bbt_coroutine_config->m_cfg_processer_steal_once_min_task_num > size / 2 ? g_bbt_coroutine_config->m_cfg_processer_steal_once_min_task_num : size / 2;

    for (auto && p : {CO_PRIORITY_CRITICAL, CO_PRIORITY_HIGH, CO_PRIORITY_NORMAL, CO_PRIORITY_LOW})
    {
        if (steal_num >= expect_size)
            break;

        size_t count = 0;
        for (size_t i = 0; i < (expect_size - steal_num); ++i)
        {
            if (!m_coroutine_queue[p].try_dequeue(item))
                break;

            AssertWithInfo(thief->m_coroutine_queue[p].enqueue(item), "oom!");
            item = nullptr;
            ++count;
        }

        steal_num += count;
    }

#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_StealSucc(steal_num);
    m_steal_count += steal_num;
#endif

    return steal_num;
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

    /* 动态绑定打断旧的 CV/Wait，让 worker 尽快进入 Tick/Wait 驱动路径 */
    _PublishRunnable();
    _WakeDriver();
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
