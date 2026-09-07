#include <cmath>
#include <cstring>
#include <cstdio>
#include <stdexcept>
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
#include <bbt/coroutine/detail/DnsResolver.hpp>
#include <bbt/coroutine/detail/debug/DebugMgr.hpp>

namespace bbt::coroutine::detail
{

Scheduler::UPtr& Scheduler::GetInstance()
{
    static UPtr _inst{nullptr};
    if (_inst == nullptr)
        _inst = UPtr(new Scheduler());
    
    return _inst;
}

Scheduler::Scheduler():
    m_down_latch(g_bbt_coroutine_config->m_cfg_static_thread_num)
{
}

Scheduler::~Scheduler()
{
    if (m_is_running.load(std::memory_order_acquire) || m_sche_thread != nullptr)
        Stop();
}

void Scheduler::_Init()
{
    m_sche_thread = nullptr;
    m_is_running.store(true, std::memory_order_release);
    m_run_status = SCHE_DEFAULT;
    m_regist_coroutine_count = 0;
    m_down_latch.Reset(g_bbt_coroutine_config->m_cfg_static_thread_num);
}

void Scheduler::RegistCoroutineTask(const CoroutineCallback& handle, const char* desc)
{
    /* 停机契约（#280）：停止接收新任务 = 明确失败。停机后 _LoadBlance2Proc
     * 必然失败，旧代码 Release 下只打 stderr、noexcept 版还回 succ=true（假
     * 成功且泄漏协程）。显式抛错让两条注册路径语义一致。 */
    if (!m_is_running.load(std::memory_order_acquire))
        throw std::runtime_error("scheduler stopped: coroutine task rejected");

    auto coroutine_sptr = Coroutine::Create(
        g_bbt_coroutine_config->m_cfg_stack_size,
        handle,
        g_bbt_coroutine_config->m_cfg_stack_protect,
        desc);

    /* 尝试先找个Processer放进执行队列，失败放入全局队列 */
    AssertWithInfo(_LoadBlance2Proc(CO_PRIORITY_NORMAL, coroutine_sptr), "this is impossible!");    
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
    /* 停机进行中（#280）：parked 回收（DestroyParkedCoroutines）接管销毁，
     * 唤醒路径不再入队——入队会与 doomed 回收形成双释放，丢弃则泄漏有界、
     * 且回调线程此后不再触碰协程栈，安全。 */
    if (!m_is_running.load(std::memory_order_acquire))
        return;
    AssertWithInfo(m_global_coroutine_queue[priority].enqueue(coroutine), "oom!");
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
    while(m_is_running.load(std::memory_order_acquire))
    {
        _OnUpdate();

        prev_scan_timepoint = prev_scan_timepoint + bbt::core::clock::ms(g_bbt_coroutine_config->m_cfg_scan_interval_ms);
        std::this_thread::sleep_until(prev_scan_timepoint);
    }
}

void Scheduler::Start(SchedulerStartOpt opt)
{
    _InitGlobalUniqInstance();
    _Init();
    bbt::core::thread::CountDownLatch wg{1};

    switch (opt) {

    case SCHE_START_OPT_SCHE_THREAD:
        Assert(m_sche_thread == nullptr);
        m_sche_thread = new std::thread([this, &wg](){
            _CreateProcessers();
            wg.Down();
            _Run();
        });
        wg.Wait();
        break;

    case SCHE_START_OPT_SCHE_NO_LOOP:
        _CreateProcessers();
        break;

    case SCHE_START_OPT_SCHE_LOOP:
        _CreateProcessers();
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


void Scheduler::Stop()
{
    /**
     * 时序约束（#231）：先停 DNS 池——Stop 会 join worker 并 Notify 全部
     * 排队/在途 Job 的 CoWaiter，唤醒路径要把协程重新投入调度队列；
     * 必须在 _DestoryProcessers() 之前执行，否则被唤醒的协程无人执行、
     * 且 worker 写回调用方协程栈时栈可能已析构。
     */
    DnsResolver::GetInstance()->Stop();

    m_is_running.store(false, std::memory_order_release);

    _DestoryProcessers();
    
    if (m_sche_thread != nullptr) {
        if (m_sche_thread->joinable())
            m_sche_thread->join();
        delete m_sche_thread;
    }

    m_sche_thread = nullptr;
    Coroutine::Ptr item = nullptr;
    /* 停机排空（#280）：此刻全部 worker、Scheduler 线程已 join，队列无并发
     * 消费者，逐个 delete 回收协程及其栈（旧代码只置空指针，Release 下泄漏
     * 每个未完成任务的对象与栈）。 */
    for (auto && queue : m_global_coroutine_queue)
        while (queue.try_dequeue(item)) {
            delete item;
            item = nullptr;
        }

    /* parked 协程（挂起在 fd/timer/custom 事件上、不在任何队列）在此统一回收：
     * 取消式停机不复活执行，直接注销事件并销毁。必须在全部线程 join 之后。 */
    Coroutine::DestroyParkedCoroutines();

    /* 兜底：swap 与 UnRegist 间隙内若有并发 Trigger（如其它线程 Hook_Close）把
     * 刚销毁的协程重新入队，此处再扫一遍删除，不留悬垂指针给下一次 Start。 */
    for (auto && queue : m_global_coroutine_queue)
        while (queue.try_dequeue(item)) {
            delete item;
            item = nullptr;
        }

    m_run_status = ScheudlerStatus::SCHE_EXIT;
#ifdef BBT_COROUTINE_PROFILE
    std::string profileinfo;
    g_bbt_profiler->ProfileInfo(profileinfo);
    bbt::core::log::DebugPrint(profileinfo.c_str());
#endif

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
            // 时序约束：必须在 latch 放行前完成初始化。latch 放行表示 Start() 即将返回，
            // 调用方随后可能立即 Stop()；若初始化延后到放行后，会把 Stop() 已发布的
            // 停止标志重置为 true，导致该 Processer 在空闲等待中永久挂起。
            processer->_Init();
            this->m_down_latch.Down();
            this->m_down_latch.Wait();
            processer->_Run();
        });
        m_proc_threads.push_back(t);
    }

    m_down_latch.Wait();
}

void Scheduler::_DestoryProcessers()
{
    /* 停止所有执行processer */
    for (auto item : m_processer_map)
        item.second->Stop();
    /* #277 review：调度线程(_FixTimingScan)持锁迭代 map，clear 必须同锁，
     * 否则 Stop 时并发 erase/iterate = UB（sche 线程此刻尚未 join） */
    {
        std::lock_guard<std::mutex> _(m_processer_map_mutex);
        m_processer_map.clear();
        m_load_blance_vec.clear();
    }

    /* 释放所有执行processer的线程 */
    for (auto&& proc_thread : m_proc_threads) {
        if (proc_thread->joinable())
            proc_thread->join();
        delete proc_thread;
    }

    m_proc_threads.clear();
}

bool Scheduler::_LoadBlance2Proc(CoroutinePriority priority, Coroutine::Ptr co)
{
    std::lock_guard<std::mutex> _(m_processer_map_mutex);
    if (m_load_blance_vec.empty())
        return false;

    uint32_t index = m_load_idx;
    m_load_idx++;

    index %= g_bbt_coroutine_config->m_cfg_static_thread_num;
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
    int max_processer_num = m_processer_map.size();

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
