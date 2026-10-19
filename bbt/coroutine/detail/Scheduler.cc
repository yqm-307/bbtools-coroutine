#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <vector>
#include <bbt/core/log/DebugPrint.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/Attribute.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>
#include <bbt/coroutine/detail/debug/DebugMgr.hpp>

namespace bbt::coroutine::detail
{

Scheduler::UPtr& Scheduler::GetInstance()
{
    /* 进程寿命（process-lifetime）策略：运行时实例与其依赖的 poller/config/
     * worker 一起活到进程结束，不在静态退出期析构。Stop 已删除，静态析构会
     * 直接销毁仍被 worker 线程使用的对象，因此这里故意让持有者泄漏——不是
     * detach 线程，也不依赖“退出时没人用”的假设（atexit 顺序不可依赖）。 */
    static UPtr* _holder = new UPtr(new Scheduler());
    return *_holder;
}

/* 构造不再读取 m_cfg_static_thread_num：旧实现把 size_t 直接窄化进 int
 * 计数的 CountDownLatch，既无法表达非法配置，也让 Start 的不可逆副作用
 * 早于校验。初始数量改由 Start 校验后显式传入。 */
Scheduler::Scheduler() = default;

/* 进程寿命：本析构实际不会被调用（实例持有者泄漏）；保留默认实现，
 * 不得在这里恢复任何停机收口。 */
Scheduler::~Scheduler() = default;

void Scheduler::_Init(const ProcesserScalingBounds& bounds)
{
    m_sche_thread = nullptr;
    m_regist_coroutine_count = 0;

    /* Start 只成功一次；这些容器在 NotStarted/Starting 阶段是空的，clear
     * 只为防御“创建失败后重置状态再重试”的路径。 */
    m_active_snapshot = std::make_shared<const ProcesserList>();
    m_processer_map.clear();
    m_workers.clear();

    m_min_num = bounds.m_min_num;
    m_max_num = bounds.m_max_num;
    m_target_num.store(bounds.m_initial_num, std::memory_order_release);
    m_scaling_mode.store(
        g_bbt_coroutine_config->m_cfg_processer_auto_scale
            ? ProcesserScalingMode::Auto
            : ProcesserScalingMode::Manual,
        std::memory_order_release);

    m_scale_interval_ms = g_bbt_coroutine_config->m_cfg_processer_scale_interval_ms;
    m_scale_up_samples = g_bbt_coroutine_config->m_cfg_processer_scale_up_samples;
    m_scale_down_samples = g_bbt_coroutine_config->m_cfg_processer_scale_down_samples;
    m_scale_step = g_bbt_coroutine_config->m_cfg_processer_scale_step;
    m_scale_up_busy_percent = g_bbt_coroutine_config->m_cfg_processer_scale_up_busy_percent;
    m_scale_down_busy_percent = g_bbt_coroutine_config->m_cfg_processer_scale_down_busy_percent;
    m_scale_up_queue_watermark = g_bbt_coroutine_config->m_cfg_processer_scale_up_queue_watermark;
    m_scale_down_queue_watermark = g_bbt_coroutine_config->m_cfg_processer_scale_down_queue_watermark;

    m_scale_up_hits = 0;
    m_scale_down_hits = 0;
    m_last_auto_sample = std::chrono::steady_clock::now();
}

void Scheduler::_PublishInitialized()
{
    /* “初始化完成”= worker 已创建并登记、poller/config 已就绪。
     * 标志必须在进入运行循环前发布：LOOP 模式（Start 不返回）下这是
     * 外部唯一能观察到初始化完成的时刻。m_start_state 停在 Starting
     * 即表示“已启动过”，重复 Start 由它拒绝。 */
    m_initialized.store(true, std::memory_order_release);
}

void Scheduler::RegistCoroutineTask(const CoroutineCallback& handle, const char* desc)
{
    /* 默认入口转调显式重载：只在入口解析默认大小与保护配置，显式重载不改写配置 */
    RegistCoroutineTask(g_bbt_coroutine_config->m_cfg_stack_size, handle, desc);
}

void Scheduler::RegistCoroutineTask(size_t stack_size, const CoroutineCallback& handle, const char* desc)
{
    /* 参数校验与栈申请（invalid_argument/length_error/bad_alloc/system_error）都发生在
     * 入队之前：创建失败不会留下已入队对象，也不改全局默认与池计数。
     * 本重载只读保护默认，不解析默认大小。 */
    Coroutine::Ptr coroutine = nullptr;
    Processer::SPtr proc = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_global_queue_mutex);
        coroutine = Coroutine::Create(
            stack_size,
            handle,
            g_bbt_coroutine_config->m_cfg_stack_protect,
            desc);
        proc = _LoadBlance2Proc(CO_PRIORITY_NORMAL, coroutine);
        if (proc != nullptr)
            coroutine = nullptr;    // 已交接给 worker：发布后不再读取可能被销毁的对象
    }

    if (coroutine != nullptr) {
        /* 无 worker 等发布前拒绝：在锁外清理，不落在可抛的清理路径里 */
        delete coroutine;
        throw std::runtime_error("scheduler has no processer: coroutine task rejected");
    }

    /* 外部 driver Wake 必须在退出 global/map/run_cond 全部锁之后执行 */
    proc->_WakeDriver();
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_RegistCoroutine();
#endif
}

void Scheduler::RegistCoroutineTaskOn(ProcesserId target, const CoroutineCallback& handle, const char* desc)
{
    RegistCoroutineTaskOn(target, g_bbt_coroutine_config->m_cfg_stack_size, handle, desc);
}

void Scheduler::RegistCoroutineTaskOn(ProcesserId target, size_t stack_size, const CoroutineCallback& handle, const char* desc)
{
    if (!IsInitialized())
        throw std::logic_error{"Scheduler::RegistCoroutineTaskOn: runtime not initialized"};

    /* 目标必须来自已登记 worker 表；GetLocalProcesser 惰性创建的未登记
     * P（如主线程）不在表内，直接拒绝，不做静默回退。 */
    Processer::SPtr proc = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_processer_map_mutex);
        auto it = m_processer_map.find(target);
        if (it != m_processer_map.end())
            proc = it->second;
    }
    if (proc == nullptr)
        throw std::invalid_argument{"Scheduler::RegistCoroutineTaskOn: invalid target processer"};

    Coroutine::Ptr coroutine = nullptr;
    bool admitted = false;
    {
        std::lock_guard<std::mutex> lock(m_global_queue_mutex);
        coroutine = Coroutine::Create(
            stack_size,
            handle,
            g_bbt_coroutine_config->m_cfg_stack_protect,
            desc);
        /* 只是初次入目标队列的偏好；steal 与恢复全局路径不改。
         * 目标可能在 map 查找后、入队前提交退役；admission gate 会拒绝，
         * 此时协程仍归本调用方，必须显式拒绝而不是写入 retired 队列。 */
        admitted = proc->AddCoroutineTask(CO_PRIORITY_NORMAL, coroutine);
        if (admitted)
            coroutine = nullptr;
    }

    if (!admitted) {
        delete coroutine;
        throw std::invalid_argument{"Scheduler::RegistCoroutineTaskOn: target processer is retiring"};
    }

    proc->_WakeDriver();
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_RegistCoroutine();
#endif
}

std::vector<ProcesserId> Scheduler::GetWorkerProcesserIds()
{
    if (!IsInitialized())
        return {};

    std::shared_ptr<const ProcesserList> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_processer_map_mutex);
        snapshot = m_active_snapshot;
    }
    if (snapshot == nullptr)
        return {};

    std::vector<ProcesserId> ids;
    ids.reserve(snapshot->size());
    for (const auto& proc : *snapshot)
        if (proc != nullptr)
            ids.push_back(proc->GetId());
    return ids;
}

ProcesserDriverStatus Scheduler::RegisterProcesserDriver(
    ProcesserId target,
    std::shared_ptr<IProcesserDriver> driver,
    ProcesserDriverToken& out_token)
{
    if (!IsInitialized())
        return ProcesserDriverStatus::NotInitialized;
    if (driver == nullptr)
        return ProcesserDriverStatus::InvalidDriver;

    Processer::SPtr proc = nullptr;
    ProcesserDriverStatus status = ProcesserDriverStatus::InvalidTarget;
    {
        /* lifecycle 互斥：绑定与退役提交共用同一线性化点。绑定先到则该 P
         * 被 pin（slot 非空），退役挑选跳过；退役先到则该 P 已移出 active，
         * 下面的查找失败并返回 InvalidTarget，out_token 不变。 */
        std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);
        {
            std::lock_guard<std::mutex> lock(m_processer_map_mutex);
            auto it = m_processer_map.find(target);
            if (it != m_processer_map.end())
                proc = it->second;
        }
        if (proc != nullptr)
            status = proc->_BindDriver(driver, out_token);
    }

    /* driver Wake 必须在退出 lifecycle/map/slot 全部锁之后执行 */
    if (status == ProcesserDriverStatus::Ok) {
        proc->_PublishRunnable();
        proc->_WakeDriver();
    }
    return status;
}

ProcesserDriverStatus Scheduler::QueryProcesserDriver(
    ProcesserId target,
    ProcesserDriverSnapshot& out_snapshot)
{
    if (!IsInitialized())
        return ProcesserDriverStatus::NotInitialized;

    Processer::SPtr proc = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_processer_map_mutex);
        auto it = m_processer_map.find(target);
        if (it != m_processer_map.end())
            proc = it->second;
    }
    if (proc == nullptr)
        return ProcesserDriverStatus::InvalidTarget;

    return proc->_QueryDriver(out_snapshot);
}

ProcesserDriverStatus Scheduler::UnregisterProcesserDriver(const ProcesserDriverToken& token)
{
    if (!IsInitialized())
        return ProcesserDriverStatus::NotInitialized;

    /* 在任何副作用前拒绝：目标 worker 线程自身、任何受管业务协程上下
     * 文——同步解绑要等 callback lease 归零，在这些上下文等待等于等
     * 自己。持该代 lease 的线程在 Processer 内部按同代身份再查一次。 */
    if (g_bbt_tls_helper->EnableUseCo() && g_bbt_tls_coroutine_co != nullptr)
        return ProcesserDriverStatus::SelfDetach;
    auto local_proc = Processer::TryGetLocalProcesser();
    if (local_proc != nullptr && local_proc->GetId() == token.m_processer_id)
        return ProcesserDriverStatus::SelfDetach;

    Processer::SPtr proc = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_processer_map_mutex);
        auto it = m_processer_map.find(token.m_processer_id);
        if (it != m_processer_map.end())
            proc = it->second;
    }
    /* 未知/过期目标一律 StaleToken：解绑只认精确同代身份，不是收据 */
    if (proc == nullptr)
        return ProcesserDriverStatus::StaleToken;

    return proc->_UnregisterDriver(token.m_generation);
}

/* -------------------------------------------------------------------------
 * 动态扩缩容控制/观察入口
 * ---------------------------------------------------------------------- */

ProcesserResizeResult Scheduler::SetProcesserNum(size_t num)
{
    if (!IsInitialized())
        return ProcesserResizeResult::NotInitialized;
    if (num < m_min_num || num > m_max_num)
        return ProcesserResizeResult::InvalidTarget;

    std::vector<Processer::SPtr> notify;
    ProcesserResizeResult result;
    {
        std::unique_lock<std::mutex> lock(m_lifecycle_mutex);
        result = _ApplyTargetLocked(num, notify, lock);
        if (result == ProcesserResizeResult::Ok ||
            result == ProcesserResizeResult::Pending) {
            /* 合法手动请求（包括 same-target）一律切 Manual 并复位样本；
             * 拒绝路径保持 target/mode/样本不变。 */
            m_scaling_mode.store(ProcesserScalingMode::Manual, std::memory_order_release);
            _ResetScalingSamplesLocked();
        }
    }

    for (auto& proc : notify)
        if (proc != nullptr)
            proc->_PublishRunnable();

    /* 手动入口也有限推进 reaper；NO_LOOP 下不依赖隐藏调度线程。 */
    _ReapRetiredWorkers();
    return result;
}

ProcesserResizeResult Scheduler::SetProcesserAutoScale(bool enable)
{
    if (!IsInitialized())
        return ProcesserResizeResult::NotInitialized;

    std::lock_guard<std::mutex> lock(m_lifecycle_mutex);
    m_scaling_mode.store(
        enable ? ProcesserScalingMode::Auto : ProcesserScalingMode::Manual,
        std::memory_order_release);
    _ResetScalingSamplesLocked();
    return ProcesserResizeResult::Ok;
}

size_t Scheduler::GetProcesserNum()
{
    if (!IsInitialized())
        return 0;
    return _ActiveCountLocked();
}

size_t Scheduler::GetProcesserTargetNum()
{
    return m_target_num.load(std::memory_order_acquire);
}

size_t Scheduler::GetProcesserRetiringNum()
{
    if (!IsInitialized())
        return 0;

    std::lock_guard<std::mutex> lock(m_processer_map_mutex);
    size_t count = 0;
    for (const auto& record : m_workers)
    {
        if (record->m_processer != nullptr &&
            record->m_processer->m_lifecycle.load(std::memory_order_acquire) ==
                ProcesserLifecycleState::Retiring)
            ++count;
    }
    return count;
}

size_t Scheduler::GetProcesserExitedUnjoinedNum()
{
    if (!IsInitialized())
        return 0;

    std::lock_guard<std::mutex> lock(m_processer_map_mutex);
    size_t count = 0;
    for (const auto& record : m_workers)
    {
        if (record->m_processer != nullptr &&
            record->m_processer->m_lifecycle.load(std::memory_order_acquire) ==
                ProcesserLifecycleState::Exited)
            ++count;
    }
    return count;
}

ProcesserScalingMode Scheduler::GetProcesserScalingMode()
{
    return m_scaling_mode.load(std::memory_order_acquire);
}

void Scheduler::_WakeProcesserDrivers()
{
    /* active 快照持有稳定强引用；这里只补发 driver Wake，不重新发布任务/latch。
     * 退役 P 已移出 active，不会被本路径唤醒或写入。 */
    auto& self = *GetInstance();
    std::shared_ptr<const ProcesserList> snapshot;
    {
        std::lock_guard<std::mutex> lock(self.m_processer_map_mutex);
        snapshot = self.m_active_snapshot;
    }
    if (snapshot == nullptr)
        return;

    for (const auto& proc : *snapshot)
        if (proc != nullptr)
            proc->_WakeDriver();
}

void Scheduler::RegistCoroutineTask(const CoroutineCallback& handle, bool& succ) noexcept
{
    try
    {
        RegistCoroutineTask(handle);
        succ = true;
    }
    catch(...)
    {
        succ = false;
        return;
    }
}

void Scheduler::RegistCoroutineTask(size_t stack_size, const CoroutineCallback& handle, bool& succ) noexcept
{
    try
    {
        RegistCoroutineTask(stack_size, handle);
        succ = true;
    }
    catch(...)
    {
        succ = false;
        return;
    }
}


void Scheduler::OnActiveCoroutine(CoroutinePriority priority, Coroutine::Ptr coroutine)
{
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->Check_IsResumedCo(coroutine->GetId());
#endif
    AssertWithInfo(priority >= CO_PRIORITY_LOW && priority < CO_PRIORITY_COUNT, "invalid priority!");
    AssertWithInfo(coroutine != nullptr, "coroutine is nullptr!");
    /* 无停机、无代际：激活的协程一律回全局队列；没有需要 reclaim 的
     * “已停止/旧代际”路径，也不再有 Stop 排空的所有权交接。 */
    {
        std::lock_guard<std::mutex> lock(m_global_queue_mutex);
        AssertWithInfo(m_global_coroutine_queue[priority].enqueue(coroutine), "oom!");
    }
    /* 只广播当前 active 快照；退役 P 已移出 active，不再接收通知。
     * 不按 PROC_SUSPEND 过滤：运行→睡眠之间的通知必须由谓词记住。
     * driver Wake 若在 deferral 作用域内只记 pending，最外层 runtime 锁
     * 释放后补发。 */
    std::shared_ptr<const ProcesserList> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_processer_map_mutex);
        snapshot = m_active_snapshot;
    }
    if (snapshot == nullptr)
        return;
    for (const auto& processer : *snapshot)
        if (processer != nullptr)
            processer->_NotifyRunnable();
}

void Scheduler::_FixTimingScan()
{
    /* worker 无进展检测（#277）：调度线程独立于 worker，每拍扫描各 worker
     * 的锁存快照。阈值=0 时整段跳过（零开销）。 */
    const size_t warn_ms = g_bbt_coroutine_config->m_cfg_worker_stall_warn_ms;
    if (warn_ms == 0)
        return;

    std::shared_ptr<const ProcesserList> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_processer_map_mutex);
        snapshot = m_active_snapshot;
    }
    if (snapshot == nullptr)
        return;

    const uint64_t now_us = bbt::core::clock::gettime_mono<bbt::core::clock::us>();
    const uint64_t threshold_us = (uint64_t)warn_ms * 1000;

    std::vector<WorkerStallInfo> pending;
    for (const auto& proc : *snapshot)
    {
        if (proc == nullptr)
            continue;
        if (!proc->m_executing.load(std::memory_order_acquire))
            continue;   // worker 空闲/协程已让出，不算停顿

        /* seqlock 读：取稳定快照 */
        uint64_t s1 = proc->m_exec_seq.load(std::memory_order_acquire);
        if (s1 & 1)
            continue;   // 正在写入，跳过一拍
        uint64_t begin_us = proc->m_exec_begin_us;
        CoroutineId co_id = proc->m_exec_co_id;
        uint64_t backlog = proc->m_exec_backlog;
        char desc[sizeof(proc->m_exec_desc)];
        memcpy(desc, proc->m_exec_desc, sizeof(desc));
        /* acquire 尾检：把字段读排序在两次 seq 读之间，消除形式数据竞争
         *（#277 review：relaxed 尾检理论上可让撕裂快照通过） */
        uint64_t s2 = proc->m_exec_seq.load(std::memory_order_acquire);
        if (s1 != s2 || begin_us == 0)
            continue;   // 读到撕裂数据

        if (now_us - begin_us < threshold_us)
            continue;

        if (proc->m_last_stall_report_begin == begin_us)
            continue;   // 同一停顿只报一次（协程仍在死循环中）
        proc->m_last_stall_report_begin = begin_us;

        WorkerStallInfo info;
        info.m_worker_id = proc->GetId();
        info.m_co_id = co_id;
        info.m_desc.assign(desc);
        info.m_running_us = now_us - begin_us;
        info.m_backlog = backlog;
        pending.push_back(std::move(info));
    }

    for (auto& info : pending)
    {
        if (g_bbt_coroutine_config->m_ext_worker_stall_callback) {
            try { g_bbt_coroutine_config->m_ext_worker_stall_callback(info); }
            catch (...) { /* 回调契约：不得抛出；抛出吞掉保调度线程 */ }
        } else {
            /* 无回调 = stderr 告警一行 */
            fprintf(stderr, "[bbtco] worker stall: worker=%llu co=%llu desc='%s' running=%llums backlog=%llu\n",
                    (unsigned long long)info.m_worker_id, (unsigned long long)info.m_co_id,
                    info.m_desc.c_str(), (unsigned long long)(info.m_running_us / 1000),
                    (unsigned long long)info.m_backlog);
        }
    }
}

void Scheduler::_OnUpdate()
{
    static auto prev_profile_timepoint = bbt::core::clock::now<>();
    bool actived = false;
    do {

        /* 自动采样与 reaper 和 poller 解耦：poll 持续有事件时也要推进。
         * 两者都不在 runtime/driver/队列锁内 join 或调用外部回调。 */
        _AutoScaleTick();
        _ReapRetiredWorkers();

#ifdef BBT_COROUTINE_PROFILE
        if (g_bbt_coroutine_config->m_cfg_profile_printf_ms > 0 &&
            bbt::core::clock::is_expired<bbt::core::clock::ms>((prev_profile_timepoint + bbt::core::clock::ms(g_bbt_coroutine_config->m_cfg_profile_printf_ms))))
        {
            std::string info = "";
            g_bbt_profiler->ProfileInfo(info);
            bbt::core::log::DebugPrint(info.c_str());
            prev_profile_timepoint = bbt::core::clock::now<>();
        }
#endif

        /* 只通过 EventLoop 门面驱动 FD/Timer/Wakeup，不直接调 epoll。
           只调 EventLoop 门面，不直连 epoll/ASIO。换 backend 不改这里。 */
        actived = g_bbt_poller->PollOnce();
        _FixTimingScan();
        g_bbt_stackpoll->OnUpdate();
    } while(actived);

}

void Scheduler::_Run()
{
    /**
     * 调度器主循环
     *
     * 1. CoPoller 进行事件轮询，检测是否有await_event就绪。
     * 2. 栈池进行定时采样和动态调整。
     *
     */

    m_begin_timestamp = bbt::core::clock::now<>();
    auto prev_scan_timepoint = bbt::core::clock::now<>();

#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_StartScheudler();
#endif
    /* 进程寿命：调度循环不设退出条件——没有业务停机入口，退出即进程结束 */
    for (;;)
    {
        _OnUpdate();

        prev_scan_timepoint = prev_scan_timepoint + bbt::core::clock::ms(g_bbt_coroutine_config->m_cfg_scan_interval_ms);
        std::this_thread::sleep_until(prev_scan_timepoint);
    }
}

void Scheduler::Start(SchedulerStartOpt opt)
{
    /* 配置校验必须在任何不可逆副作用（Start 状态转移、线程、worker 登记）
     * 之前完成；失败时输出不变、无副作用，修正配置后可重试。 */
    ProcesserScalingBounds bounds{};
    if (!g_bbt_coroutine_config->ResolveProcesserScalingBounds(bounds))
        throw std::invalid_argument{"Scheduler::Start: invalid processer scaling configuration"};

    {
        /* Start 一次性：状态检查与转移同锁，拒绝重复与并发 Start。
         * 不能用布尔字段代替同步——两个线程同时读到 false 会双双进入初始化。 */
        std::lock_guard<std::mutex> lock(m_start_mutex);
        if (m_start_state != StartState::NotStarted)
            throw std::logic_error{"Scheduler::Start: runtime already started"};
        m_start_state = StartState::Starting;
    }

    try {
        _InitGlobalUniqInstance();
        _Init(bounds);
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_start_mutex);
        m_start_state = StartState::NotStarted;
        throw;
    }

    switch (opt) {

    case SCHE_START_OPT_SCHE_THREAD:
    {
        Assert(m_sche_thread == nullptr);
        bbt::core::thread::CountDownLatch wg{1};
        std::shared_ptr<std::atomic_bool> created;
        const size_t initial_num = bounds.m_initial_num;
        try {
            created = std::make_shared<std::atomic_bool>(false);
            m_sche_thread = new std::thread([this, &wg, created, initial_num](){
                const bool ok = _CreateProcessers(initial_num);
                created->store(ok, std::memory_order_release);
                /* 初始化标志先于运行循环发布；latch 放行表示 Start 即将返回 */
                if (ok)
                    _PublishInitialized();
                wg.Down();
                if (ok)
                    _Run();
            });
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_start_mutex);
            m_start_state = StartState::NotStarted;
            throw;
        }
        wg.Wait();

        if (!created->load(std::memory_order_acquire)) {
            if (m_sche_thread->joinable())
                m_sche_thread->join();
            delete m_sche_thread;
            m_sche_thread = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_start_mutex);
                m_start_state = StartState::NotStarted;
            }
            throw std::runtime_error{"Scheduler::Start: failed to create processers"};
        }
        break;
    }

    case SCHE_START_OPT_SCHE_NO_LOOP:
        if (!_CreateProcessers(bounds.m_initial_num)) {
            {
                std::lock_guard<std::mutex> lock(m_start_mutex);
                m_start_state = StartState::NotStarted;
            }
            throw std::runtime_error{"Scheduler::Start: failed to create processers"};
        }
        _PublishInitialized();
        break;

    case SCHE_START_OPT_SCHE_LOOP:
        if (!_CreateProcessers(bounds.m_initial_num)) {
            {
                std::lock_guard<std::mutex> lock(m_start_mutex);
                m_start_state = StartState::NotStarted;
            }
            throw std::runtime_error{"Scheduler::Start: failed to create processers"};
        }
        _PublishInitialized();
        _Run();
        break;

    default:
        break;
    };

}

void Scheduler::LoopOnce()
{
    // 使用单独的调度线程，就不可以调用LoopOnce来手动驱动了
    AssertWithInfo(m_sche_thread == nullptr, "the sche-thread has been started!");

    _OnUpdate();
}

size_t Scheduler::GetCoroutineFromGlobal(CoroutinePriority priority, CoroutineQueue& queue, size_t size, Processer* owner)
{
    size_t count = 0;
    Coroutine::Ptr item = nullptr;

    for (size_t i = 0; i < size; ++i) {
        if (!m_global_coroutine_queue[priority].try_dequeue(item))
            break;

        if (owner != nullptr) {
            /* 逐项重新过 admission gate：worker 在拉取过程中被提交退役时，
             * 已出队的任务必须保留所有权。notify=false：调用方可能正持有
             * 自己的 run_cond（睡前复查），不能重入 _PublishRunnable；也不能
             * 取 global 锁（会在 run_cond 内形成 global→map→run_cond→global
             * 环）。gate 关闭时直接放回本地队列——本 worker 随后必走 drain，
             * 与自身 drain 顺序执行，无丢失/重复。 */
            if (!owner->_EnqueueAdmitted(priority, item, /*notify*/false)) {
                AssertWithInfo(owner->m_coroutine_queue[priority].enqueue(item), "oom!");
            }
        } else {
            AssertWithInfo(queue.enqueue(item), "oom!");
        }
        item = nullptr;
        ++count;
    }

    return count;
}

bool Scheduler::_CreateProcessers(size_t initial_num)
{
    std::unique_lock<std::mutex> lock(m_lifecycle_mutex);
    std::vector<Processer::SPtr> created;
    if (!_PrepareExpansionLocked(initial_num, created, lock))
        return false;

    m_target_num.store(initial_num, std::memory_order_release);
    return true;
}

void Scheduler::_WorkerMain(WorkerRecordSPtr record, std::shared_ptr<BatchStartGate> gate)
{
    bool ready = true;
    try {
        /* 在发布批次前安装 TLS 和线程退出收据；注册本身也可能分配失败。
         * future 只在包括用户 TLS 析构在内的线程退出阶段才就绪，不能用
         * Processer::Exited 提前证明 join 不会阻塞事件驱动线程。 */
        record->m_exit_promise.set_value_at_thread_exit();
        g_bbt_tls_helper->SetEnableUseCo(true);
        Processer::_InstallLocalProcesser(record->m_processer);
    } catch (...) {
        ready = false;
    }
    {
        std::unique_lock<std::mutex> lock(gate->m_mutex);
        ++gate->m_ready_num;
        gate->m_failed = gate->m_failed || !ready;
        gate->m_cv.notify_all();
        if (!ready)
            return;
        gate->m_cv.wait(lock, [&] {
            return gate->m_started || gate->m_cancel;
        });
        if (gate->m_cancel)
            return;
    }

    /* 新线程的 TLS 必须指向 prepare 阶段创建、commit 阶段已登记的同一个 P；
     * 不能在这里惰性创建第二个 P，否则 registry/Profiler 会出现孤儿。 */
    record->m_processer->_Run();
}

void Scheduler::_CancelBatch(std::shared_ptr<BatchStartGate>& gate,
                             std::vector<WorkerRecordSPtr>& records)
{
    {
        std::lock_guard<std::mutex> lock(gate->m_mutex);
        gate->m_cancel = true;
    }
    gate->m_cv.notify_all();

    for (auto& record : records)
    {
        if (record == nullptr || record->m_thread == nullptr)
            continue;
        if (record->m_thread->joinable()) {
            try { record->m_thread->join(); }
            catch (...) { /* 未公开批次：join 失败也不改变旧 active/target */ }
        }
        delete record->m_thread;
        record->m_thread = nullptr;
        record->m_processer.reset();
    }
    records.clear();
}

bool Scheduler::_PrepareExpansionLocked(size_t need, std::vector<Processer::SPtr>& out_new,
                                        std::unique_lock<std::mutex>& lifecycle_lock)
{
    if (need == 0)
        return true;

    std::shared_ptr<BatchStartGate> gate;
    std::vector<WorkerRecordSPtr> records;

    /* 准备阶段：只创建 P/线程并让线程停在 gate 前。任何失败只撤销本批，
     * 不触碰旧 active/target/mode；线程尚未登记，不会接收用户任务。 */
    try {
        gate = std::make_shared<BatchStartGate>();
        records.reserve(need);
        out_new.reserve(need);
        for (size_t i = 0; i < need; ++i) {
            auto proc = Processer::Create();
            proc->_Init();
            proc->m_lifecycle.store(ProcesserLifecycleState::Preparing, std::memory_order_release);
            proc->m_admission_open.store(false, std::memory_order_release);

            auto record = std::make_shared<WorkerRecord>();
            record->m_processer = proc;
            record->m_thread = new std::thread([this, record, gate](){
                _WorkerMain(record, gate);
            });

            records.push_back(record);
            out_new.push_back(proc);
        }
        {
            std::unique_lock<std::mutex> lock(gate->m_mutex);
            gate->m_cv.wait(lock, [&] { return gate->m_ready_num == need; });
            if (gate->m_failed)
                throw std::runtime_error("processer thread initialization failed");
        }

        /* 提交前所有线程已完成可失败的初始化；以下分配仍属准备阶段。
         * swap/append/激活开始后不再分配，不公开半批 worker。 */
        std::lock_guard<std::mutex> lock(m_processer_map_mutex);
        auto new_map = m_processer_map;
        ProcesserList new_vec = *m_active_snapshot;
        new_vec.reserve(new_vec.size() + need);
        for (auto& record : records) {
            new_map.emplace(record->m_processer->GetId(), record->m_processer);
            new_vec.push_back(record->m_processer);
        }
        auto new_snapshot = std::make_shared<const ProcesserList>(std::move(new_vec));
        m_workers.reserve(m_workers.size() + need);

#ifdef BBT_COROUTINE_PROFILE
        /* 诊断 registry 也属于准备阶段：任一分配失败撤销未公开批次。
         * 同时持有两份 registry 锁后整体 swap，读者看不到漏登的半批。 */
        auto& profiler = *g_bbt_profiler;
        std::unique_lock<std::mutex> profile_lock(profiler.m_processer_map_mutex);
        auto new_profile_map = profiler.m_processer_map;
        for (const auto& record : records)
            new_profile_map.emplace(record->m_processer->GetId(), record->m_processer);
#endif
        m_processer_map.swap(new_map);
        m_active_snapshot = new_snapshot;
#ifdef BBT_COROUTINE_PROFILE
        profiler.m_processer_map.swap(new_profile_map);
#endif
        for (auto& record : records)
            m_workers.push_back(record);
        for (auto& record : records)
            record->m_processer->_Activate();
    } catch (...) {
        /* 失败批次不在 active 中。取消/join 时释放 lifecycle，避免锁内
         * 运行线程退出/TLS 清理；失败请求本身不修改当前目标或模式。 */
        if (gate != nullptr) {
            lifecycle_lock.unlock();
            _CancelBatch(gate, records);
            lifecycle_lock.lock();
        }
        out_new.clear();
        return false;
    }


    {
        std::lock_guard<std::mutex> lock(gate->m_mutex);
        gate->m_started = true;
    }
    gate->m_cv.notify_all();
    return true;
}

ProcesserResizeResult Scheduler::_CommitShrinkLocked(size_t need, size_t target,
                                                     std::vector<Processer::SPtr>& out_notify)
{
    Processer::SPtr self = Processer::TryGetLocalProcesser();
    const ProcesserId self_id = self != nullptr
        ? self->GetId()
        : BBT_COROUTINE_INVALID_PROCESSER_ID;

    std::lock_guard<std::mutex> lock(m_processer_map_mutex);
    const ProcesserList& active = *m_active_snapshot;

    /* 候选必须 active 且没有 Bound/Failed/Detaching 的 driver 归属；
     * slot 非空即 pin，直到同代解绑真正清槽。 */
    std::vector<Processer::SPtr> candidates;
    std::vector<Processer::SPtr> selected;
    try {
        candidates.reserve(active.size());
        for (const auto& proc : active)
        {
            if (proc == nullptr)
                continue;
            if (proc->m_lifecycle.load(std::memory_order_acquire) !=
                ProcesserLifecycleState::Active)
                continue;
            if (proc->_HasDriverOwnership())
                continue;
            candidates.push_back(proc);
        }

        if (candidates.size() < need)
            return ProcesserResizeResult::DriverBound;

        /* worker 上下文调用时，若必须退役自身，返回 SelfRetire 且无副作用；
         * 其余候选足够时仍允许本次缩容。 */
        selected.reserve(need);
        for (const auto& proc : candidates)
        {
            if (selected.size() >= need)
                break;
            if (proc->GetId() == self_id)
                continue;
            selected.push_back(proc);
        }
        if (selected.size() < need)
            return ProcesserResizeResult::SelfRetire;

        if (_CountOutstandingRetireLocked() + need > kMaxOutstandingRetire)
            return ProcesserResizeResult::Busy;

        /* 先构造新 map/snapshot（仍可抛），再一次性提交；失败无副作用。 */
        std::unordered_set<ProcesserId> selected_ids;
        selected_ids.reserve(need);
        for (const auto& proc : selected)
            selected_ids.insert(proc->GetId());

        auto new_map = m_processer_map;
        ProcesserList new_vec;
        new_vec.reserve(active.size() - need);
        for (const auto& proc : active)
        {
            if (selected_ids.count(proc->GetId()) == 0)
                new_vec.push_back(proc);
        }
        auto new_snapshot = std::make_shared<const ProcesserList>(std::move(new_vec));

        for (auto& proc : selected) {
            new_map.erase(proc->GetId());
            proc->_BeginRetire();
        }
        m_processer_map.swap(new_map);
        m_active_snapshot = new_snapshot;
        m_target_num.store(target, std::memory_order_release);
    } catch (...) {
        return ProcesserResizeResult::Busy;
    }

    out_notify = std::move(selected);
    return ProcesserResizeResult::Pending;
}

ProcesserResizeResult Scheduler::_ApplyTargetLocked(size_t target,
                                                    std::vector<Processer::SPtr>& out_notify,
                                                    std::unique_lock<std::mutex>& lifecycle_lock)
{
    const size_t active = _ActiveCountLocked();

    if (target > active) {
        const size_t need = target - active;
        if (!_PrepareExpansionLocked(need, out_notify, lifecycle_lock))
            return ProcesserResizeResult::CreateFailed;
        m_target_num.store(target, std::memory_order_release);
        return ProcesserResizeResult::Ok;
    }

    if (target < active)
        return _CommitShrinkLocked(active - target, target, out_notify);

    m_target_num.store(target, std::memory_order_release);
    return ProcesserResizeResult::Ok;
}

size_t Scheduler::_ActiveCountLocked()
{
    std::lock_guard<std::mutex> lock(m_processer_map_mutex);
    return m_active_snapshot != nullptr ? m_active_snapshot->size() : 0;
}

size_t Scheduler::_CountOutstandingRetireLocked()
{
    size_t count = 0;
    for (const auto& record : m_workers)
    {
        if (record->m_processer == nullptr)
            continue;
        const auto state = record->m_processer->m_lifecycle.load(std::memory_order_acquire);
        if (state == ProcesserLifecycleState::Retiring ||
            state == ProcesserLifecycleState::Exited)
            ++count;
    }
    return count;
}

void Scheduler::_ResetScalingSamplesLocked()
{
    m_scale_up_hits = 0;
    m_scale_down_hits = 0;
    m_last_auto_sample = std::chrono::steady_clock::now();
}

void Scheduler::_AutoScaleTick()
{
    if (!IsInitialized())
        return;
    if (m_scaling_mode.load(std::memory_order_acquire) != ProcesserScalingMode::Auto)
        return;

    std::vector<Processer::SPtr> notify;
    {
        std::unique_lock<std::mutex> lock(m_lifecycle_mutex);
        if (m_scaling_mode.load(std::memory_order_acquire) != ProcesserScalingMode::Auto)
            return;

        const auto now = std::chrono::steady_clock::now();
        const auto interval = std::chrono::milliseconds(m_scale_interval_ms);
        if (now - m_last_auto_sample < interval)
            return;
        m_last_auto_sample = now;

        std::shared_ptr<const ProcesserList> snapshot;
        {
            std::lock_guard<std::mutex> map_lock(m_processer_map_mutex);
            snapshot = m_active_snapshot;
        }
        if (snapshot == nullptr || snapshot->empty())
            return;

        /* 近似 runnable：全局 + active 本地。只作控制信号，不证明空队列、
         * 所有权移交或安全退役。 */
        uint64_t queue_total = 0;
        for (auto& queue : m_global_coroutine_queue)
            queue_total += static_cast<uint64_t>(queue.size_approx());

        size_t busy = 0;
        for (const auto& proc : *snapshot)
        {
            if (proc == nullptr)
                continue;
            queue_total += static_cast<uint64_t>(proc->GetExecutableNum());
            if (proc->m_busy.load(std::memory_order_acquire))
                ++busy;
        }

        const size_t active = snapshot->size();
        const size_t busy_percent = active == 0
            ? 0
            : static_cast<size_t>((static_cast<uint64_t>(busy) * 100) / active);

        const bool up_hit =
            queue_total >= m_scale_up_queue_watermark ||
            busy_percent >= m_scale_up_busy_percent;
        const bool down_hit =
            queue_total <= m_scale_down_queue_watermark &&
            busy_percent <= m_scale_down_busy_percent;

        if (up_hit) {
            if (m_scale_up_hits < m_scale_up_samples)
                ++m_scale_up_hits;
        } else {
            m_scale_up_hits = 0;
        }

        if (down_hit) {
            if (m_scale_down_hits < m_scale_down_samples)
                ++m_scale_down_hits;
        } else {
            m_scale_down_hits = 0;
        }

        /* 相等阈值同拍双向命中时只选扩容方向；每拍至多一批。 */
        if (m_scale_up_hits >= m_scale_up_samples) {
            const size_t target = m_target_num.load(std::memory_order_acquire);
            if (target < m_max_num) {
                const size_t remain = m_max_num - target;
                const size_t next = m_scale_step >= remain
                    ? m_max_num
                    : target + m_scale_step;
                const auto result = _ApplyTargetLocked(next, notify, lock);
                if (result == ProcesserResizeResult::Ok) {
                    m_scale_up_hits = 0;
                    m_scale_down_hits = 0;
                } else {
                    m_scale_up_hits = 0;
                }
            } else {
                m_scale_up_hits = 0;
            }
        } else if (m_scale_down_hits >= m_scale_down_samples) {
            const size_t target = m_target_num.load(std::memory_order_acquire);
            if (target > m_min_num) {
                const size_t remain = target - m_min_num;
                const size_t next = m_scale_step >= remain
                    ? m_min_num
                    : target - m_scale_step;
                const auto result = _ApplyTargetLocked(next, notify, lock);
                if (result == ProcesserResizeResult::Ok ||
                    result == ProcesserResizeResult::Pending) {
                    m_scale_up_hits = 0;
                    m_scale_down_hits = 0;
                } else {
                    /* DriverBound/Busy/SelfRetire/CreateFailed：目标不变，
                     * 复位本方向，等下一轮重新累计。 */
                    m_scale_down_hits = 0;
                }
            } else {
                m_scale_down_hits = 0;
            }
        }
    }

    /* worker 唤醒与 driver Wake 在 lifecycle/map 锁外执行 */
    for (auto& proc : notify)
        if (proc != nullptr)
            proc->_PublishRunnable();
}

void Scheduler::_ReapRetiredWorkers()
{
    if (!IsInitialized())
        return;

    /* 一次只认领一个已完成 TLS 析构的线程，无临时 vector 分配：OOM 不会
     * 留下“已认领但没有收据”的记录。Exited 仍是未 join 的可观察状态；
     * 退出 future 未就绪时跳过，不能阻塞 S/LoopOnce 等待用户 TLS。 */
    for (size_t reaped = 0; reaped < kMaxReapPerTick; ++reaped)
    {
        WorkerRecordSPtr claimed;
        {
            std::lock_guard<std::mutex> lock(m_processer_map_mutex);
            for (const auto& record : m_workers)
            {
                if (record->m_claimed.load(std::memory_order_acquire) ||
                    record->m_processer->m_lifecycle.load(std::memory_order_acquire) !=
                        ProcesserLifecycleState::Exited ||
                    record->m_thread_exited.wait_for(std::chrono::seconds(0)) !=
                        std::future_status::ready)
                    continue;
                record->m_claimed.store(true, std::memory_order_release);
                claimed = record;
                break;
            }
        }
        if (claimed == nullptr)
            return;

        try {
            if (claimed->m_thread->joinable())
                claimed->m_thread->join();
        } catch (...) {
            claimed->m_claimed.store(false, std::memory_order_release);
            return;
        }
#ifdef BBT_COROUTINE_PROFILE
        g_bbt_profiler->OnEvent_StopPorcesser(claimed->m_processer);
#endif
        delete claimed->m_thread;
        claimed->m_thread = nullptr;
        claimed->m_processer->_MarkJoined();
        {
            std::lock_guard<std::mutex> lock(m_processer_map_mutex);
            for (auto it = m_workers.begin(); it != m_workers.end(); ++it)
            {
                if (it->get() == claimed.get()) {
                    m_workers.erase(it);
                    break;
                }
            }
        }
        /* 最后一个 Processer 强引用在全部锁外释放。 */
        claimed->m_processer.reset();
    }
}

void Scheduler::_WakeActiveWorkers()
{
    std::shared_ptr<const ProcesserList> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_processer_map_mutex);
        snapshot = m_active_snapshot;
    }
    if (snapshot == nullptr)
        return;

    for (const auto& proc : *snapshot)
        if (proc != nullptr)
            proc->_NotifyRunnable();
}

bool Scheduler::_EnqueueGlobal(CoroutinePriority priority, Coroutine::Ptr coroutine)
{
    if (coroutine == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(m_global_queue_mutex);
    return m_global_coroutine_queue[priority].enqueue(coroutine);
}

Processer::SPtr Scheduler::_LoadBlance2Proc(CoroutinePriority priority, Coroutine::Ptr co)
{
    std::lock_guard<std::mutex> _(m_processer_map_mutex);
    if (m_active_snapshot == nullptr || m_active_snapshot->empty())
        return nullptr;

    const ProcesserList& vec = *m_active_snapshot;
    const size_t count = vec.size();
    const uint32_t start = m_load_idx.fetch_add(1, std::memory_order_relaxed);

    /* 按实际登记表容量取模，不能读 m_cfg_static_thread_num：worker 数在
     * 运行期可变化，m_active_snapshot 才是权威；候选若刚好在入队前关闭
     * admission，继续尝试下一个 active，全部失败才回全局/拒绝。 */
    for (size_t i = 0; i < count; ++i) {
        const size_t index = (start + i) % count;
        auto proc = vec[index];
        if (proc == nullptr)
            continue;
        if (proc->AddCoroutineTask(priority, co))
            return proc;
    }

    return nullptr;
}

int Scheduler::TryWorkSteal(Processer::SPtr thief)
{
    /**
     * m_active_snapshot 在提交时整体替换；快照只保对象寿命，真正窃取前
     * 仍要检查双方 lifecycle/admission。thief 正在退役时不再收入新任务。
     */
    if (thief == nullptr || !thief->_IsActive())
        return 0;

    std::shared_ptr<const ProcesserList> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_processer_map_mutex);
        snapshot = m_active_snapshot;
    }
    if (snapshot == nullptr || snapshot->size() <= 1)
        return 0;

    const size_t max_processer_num = snapshot->size();
    const uint32_t start = m_steal_idx.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < max_processer_num; i++) {
        const size_t index = (start + i) % max_processer_num;
        auto proc = (*snapshot)[index];
        if (proc == nullptr || proc->GetId() == thief->GetId())
            continue;
        if (!proc->_IsActive())
            continue;

        const int steal_num = static_cast<int>(proc->Steal(thief));
        if (steal_num > 0)
            return steal_num;
    }

    return 0;
}

void Scheduler::_InitGlobalUniqInstance()
{
    /**
     * 禁止编译器优化。
     *
     * 1. 确保全局单例在使用前被初始化，防止编译器将全局单例的初始化延迟到第一次使用时。
     * 2. 确保全局单例在程序启动时就被初始化，避免多线程环境下的竞态条件。
     * 3. 确保全局单例的初始化顺序正确，避免依赖关系错误。
     *
     */
#pragma optimize("", off)
    BBTATTR_COMM_UNUSED auto& _init_tls_helper = g_bbt_tls_helper;
    BBTATTR_COMM_UNUSED auto& _init_poller = g_bbt_poller;
    BBTATTR_COMM_UNUSED auto& _init_profile = g_bbt_profiler;
    BBTATTR_COMM_UNUSED auto& _init_stackpool = g_bbt_stackpoll;
#pragma optimize("", on)
}

} // namespace bbt::coroutine::detail
