#include <cmath>
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
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/debug/DebugMgr.hpp>
#include <bbt/coroutine/detail/Trace.hpp>

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
}

void Scheduler::_Init()
{
    m_sche_thread = nullptr;
    m_is_running.store(true, std::memory_order_release);
    m_run_status.store(SCHE_DEFAULT, std::memory_order_release);
    m_load_idx.store(0, std::memory_order_release);
    m_steal_idx.store(0, std::memory_order_release);
    m_regist_coroutine_count = 0;
    m_down_latch.Reset(g_bbt_coroutine_config->m_cfg_static_thread_num);
}

void Scheduler::RegistCoroutineTask(const CoroutineCallback& handle, const std::string& desc)
{
    if (!IsRunning())
        throw std::runtime_error("scheduler is not running");

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

void Scheduler::RegistCoroutineTask(const CoroutineCallback& handle, bool& succ, const std::string& desc) noexcept
{
    try
    {
        RegistCoroutineTask(handle, desc);
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


void Scheduler::OnActiveCoroutine(CoroutinePriority priority, Coroutine::Ptr coroutine, int reason)
{
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->Check_IsResumedCo(coroutine->GetId());
#endif
    AssertWithInfo(priority >= CO_PRIORITY_LOW && priority < CO_PRIORITY_COUNT, "invalid priority!");
    AssertWithInfo(coroutine != nullptr, "coroutine is nullptr!");

    if (!IsRunning()) {
        coroutine->FinalizeForTeardown();
        delete coroutine;
        return;
    }

    coroutine->SetLastResumeReason(reason);
    AssertWithInfo(m_global_coroutine_queue[priority].enqueue(coroutine), "oom!");
    g_bbt_trace->OnCoroutineEnqueue(coroutine, BBT_COROUTINE_INVALID_PROCESSER_ID, reason);
}

void Scheduler::_FixTimingScan()
{
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
    m_run_status.store(SCHE_RUNNING, std::memory_order_release);
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
    if (g_bbt_tls_processer != nullptr) {
        throw std::runtime_error("scheduler stop cannot be called from coroutine worker context");
    }

    if (!m_is_running.exchange(false, std::memory_order_acq_rel))
        return;

    m_run_status.store(ScheudlerStatus::SCHE_STOPPING, std::memory_order_release);

    for (auto& item : m_processer_map)
        item.second->Stop();
    
    if (m_sche_thread != nullptr) {
        if (m_sche_thread->joinable())
            m_sche_thread->join();
        delete m_sche_thread;
    }

    m_sche_thread = nullptr;

    _DestoryProcessers();
    _DrainGlobalCoroutineQueue();

    m_run_status.store(ScheudlerStatus::SCHE_EXIT, std::memory_order_release);
#ifdef BBT_COROUTINE_PROFILE
    std::string profileinfo;
    g_bbt_profiler->ProfileInfo(profileinfo);
    bbt::core::log::DebugPrint(profileinfo.c_str());
#endif

}

void Scheduler::_DrainGlobalCoroutineQueue()
{
    Coroutine::Ptr item = nullptr;
    for (auto&& queue : m_global_coroutine_queue) {
        while (queue.try_dequeue(item)) {
            item->FinalizeForTeardown();
            delete item;
            item = nullptr;
        }
    }
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
            this->m_down_latch.Down();
            this->m_down_latch.Wait();
            processer->Start(false);
        });
        m_proc_threads.push_back(t);
    }

    m_down_latch.Wait();
}

void Scheduler::_DestoryProcessers()
{
    /* 释放所有执行processer的线程 */
    for (auto&& proc_thread : m_proc_threads) {
        if (proc_thread->joinable())
            proc_thread->join();
        delete proc_thread;
    }

    m_proc_threads.clear();
    m_processer_map.clear();
    m_load_blance_vec.clear();
}

bool Scheduler::_LoadBlance2Proc(CoroutinePriority priority, Coroutine::Ptr co)
{
    if (m_load_blance_vec.empty())
        return false;

    uint32_t index = m_load_idx.fetch_add(1, std::memory_order_relaxed);

    index %= g_bbt_coroutine_config->m_cfg_static_thread_num;
    auto proc = m_load_blance_vec[index];
    proc->AddCoroutineTask(priority, co, TRACE_REASON_CREATE);

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
    uint32_t index = m_steal_idx.load(std::memory_order_relaxed);
    int steal_num = 0;
    int max_processer_num = m_processer_map.size();

    for (int i = 0; i < max_processer_num; i++) {
        index = m_steal_idx.fetch_add(1, std::memory_order_relaxed) % max_processer_num;
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
