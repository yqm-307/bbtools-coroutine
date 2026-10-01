#include <cmath>
#include <cstring>
#include <cstdio>
#include <memory>
#include <stdexcept>
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

Scheduler::Scheduler():
    m_down_latch(g_bbt_coroutine_config->m_cfg_static_thread_num)
{
}

/* 进程寿命：本析构实际不会被调用（实例持有者泄漏）；保留默认实现，
 * 不得在这里恢复任何停机收口。 */
Scheduler::~Scheduler() = default;

void Scheduler::_Init()
{
    m_sche_thread = nullptr;
    m_regist_coroutine_count = 0;
    m_down_latch.Reset(g_bbt_coroutine_config->m_cfg_static_thread_num);
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
    Coroutine::Ptr coroutine = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_global_queue_mutex);
        coroutine = Coroutine::Create(
            g_bbt_coroutine_config->m_cfg_stack_size,
            handle,
            g_bbt_coroutine_config->m_cfg_stack_protect,
            desc);
        if (!_LoadBlance2Proc(CO_PRIORITY_NORMAL, coroutine)) {
            delete coroutine;
            throw std::runtime_error("scheduler has no processer: coroutine task rejected");
        }
    }
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_RegistCoroutine();
#endif
}

void Scheduler::RegistCoroutineTask(const CoroutineCallback& handle, bool& succ) noexcept
{
    try
    {
        RegistCoroutineTask(handle);
        succ = true;
    }
    catch(std::runtime_error& e)
    {
        // std::cerr << "[bbtco] " << e.what() << std::endl;
        succ = false;
        return;
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
}

void Scheduler::_FixTimingScan()
{
    /* worker 无进展检测（#277）：调度线程独立于 worker，每拍扫描各 worker
     * 的锁存快照。阈值=0 时整段跳过（零开销）。 */
    const size_t warn_ms = g_bbt_coroutine_config->m_cfg_worker_stall_warn_ms;
    if (warn_ms == 0)
        return;

    const uint64_t now_us = bbt::core::clock::gettime_mono<bbt::core::clock::us>();
    const uint64_t threshold_us = (uint64_t)warn_ms * 1000;

    std::vector<WorkerStallInfo> pending;
    {
    /* 锁内只收集快照；用户回调一律出锁调用——回调若触达 Scheduler API
     * （同线程重入）会造成自死锁，审查（deleg_a2f79818）判定 Important。 */
    std::lock_guard<std::mutex> _(m_processer_map_mutex);
    for (auto&& [pid, proc] : m_processer_map)
    {
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
        info.m_worker_id = pid;
        info.m_co_id = co_id;
        info.m_desc.assign(desc);
        info.m_running_us = now_us - begin_us;
        info.m_backlog = backlog;
        pending.push_back(std::move(info));
    }
    } /* 出锁 */

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
    {
        /* Start 一次性：状态检查与转移同锁，拒绝重复与并发 Start。
         * 不能用布尔字段代替同步——两个线程同时读到 false 会双双进入初始化。 */
        std::lock_guard<std::mutex> lock(m_start_mutex);
        if (m_start_state != StartState::NotStarted)
            throw std::logic_error{"Scheduler::Start: runtime already started"};
        m_start_state = StartState::Starting;
    }

    _InitGlobalUniqInstance();
    _Init();
    bbt::core::thread::CountDownLatch wg{1};

    switch (opt) {

    case SCHE_START_OPT_SCHE_THREAD:
        Assert(m_sche_thread == nullptr);
        m_sche_thread = new std::thread([this, &wg](){
            _CreateProcessers();
            /* 初始化标志先于运行循环发布；latch 放行表示 Start 即将返回 */
            _PublishInitialized();
            wg.Down();
            _Run();
        });
        wg.Wait();
        break;

    case SCHE_START_OPT_SCHE_NO_LOOP:
        _CreateProcessers();
        _PublishInitialized();
        break;

    case SCHE_START_OPT_SCHE_LOOP:
        _CreateProcessers();
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

size_t Scheduler::GetCoroutineFromGlobal(CoroutinePriority priority, CoroutineQueue& queue, size_t size)
{
    size_t count = 0;
    Coroutine::Ptr item = nullptr;

    for (size_t i = 0; i < size; ++i) {
        if (!m_global_coroutine_queue[priority].try_dequeue(item))
            break;

        AssertWithInfo(queue.enqueue(item), "oom!");
        item = nullptr;
        ++count;
    }

    return count;
}

void Scheduler::_CreateProcessers()
{
    int need_create_thread_num = g_bbt_coroutine_config->m_cfg_static_thread_num - m_processer_map.size();
    for (int i = 0; i < need_create_thread_num; ++i)
    {
        auto t = new std::thread([this](){
            g_bbt_tls_helper->SetEnableUseCo(true);
            auto processer = g_bbt_tls_processer;
            Assert(processer != nullptr);
            {
                std::lock_guard<std::mutex> _(this->m_processer_map_mutex);
                this->m_processer_map.insert(std::make_pair(processer->GetId(), processer));
                m_load_blance_vec.push_back(processer);
            }
            // 时序约束：必须在 latch 放行前完成初始化。latch 放行表示 Start() 即将
            // 返回，外部可能立刻开始注册/驱动协程；若初始化延后到放行后，
            // Processer 会在尚未就绪时被投递任务。
            processer->_Init();
            this->m_down_latch.Down();
            this->m_down_latch.Wait();
            processer->_Run();
        });
        m_proc_threads.push_back(t);
    }

    m_down_latch.Wait();
}

bool Scheduler::_LoadBlance2Proc(CoroutinePriority priority, Coroutine::Ptr co)
{
    std::lock_guard<std::mutex> _(m_processer_map_mutex);
    if (m_load_blance_vec.empty())
        return false;

    uint32_t index = m_load_idx;
    m_load_idx++;

    /* 按实际登记表容量取模，不能读 m_cfg_static_thread_num：worker 数在
     * Start 时按当次配置建成，此后 m_load_blance_vec 才是权威；配置项在
     * Start 之后被外部改写会让下标越界（验收发现的多 worker 崩溃：
     * std::vector<shared_ptr<Processer>>::operator[] 断言）。 */
    index %= m_load_blance_vec.size();
    auto proc = m_load_blance_vec[index];
    proc->AddCoroutineTask(priority, co);

    return true;
}

int Scheduler::TryWorkSteal(Processer::SPtr thief)
{
    /**
     * m_processer_map 在主线程初始化后都是安全的
     * 
     * m_steal_idx 是一个全局的索引，所有线程都可以访问，且只
     * 用来loadblance，不需要保证完全可靠，其实int32大部分情况
     * 都算是原子的
     * 
     */
    uint32_t index = m_steal_idx;
    int steal_num = 0;
    /* 与 _LoadBlance2Proc 同口径：窃取也以实际登记表容量为准 */
    int max_processer_num = static_cast<int>(m_load_blance_vec.size());
    if (max_processer_num <= 0)
        return 0;

    for (int i = 0; i < max_processer_num; i++) {
        m_steal_idx++;
        index = m_steal_idx % max_processer_num;
        auto proc = m_load_blance_vec[index];
        Assert(proc != nullptr);
        if (proc->GetId() == thief->GetId())
            continue;

        steal_num = proc->Steal(thief);
        if (steal_num > 0)
            break;
    }

    return steal_num;
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
