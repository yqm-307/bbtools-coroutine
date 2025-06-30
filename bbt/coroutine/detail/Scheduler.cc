#include <cmath>
#include <bbt/core/log/DebugPrint.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
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
}

void Scheduler::_Init()
{
    m_sche_thread = nullptr;
    m_is_running = true;
    m_run_status = SCHE_DEFAULT;
    m_regist_coroutine_count = 0;
    m_down_latch.Reset(g_bbt_coroutine_config->m_cfg_static_thread_num);
}

void Scheduler::RegistCoroutineTask(const CoroutineCallback& handle)
{
#ifdef BBT_COROUTINE_PROFILE
    auto coroutine_sptr = Coroutine::Create(
        g_bbt_coroutine_config->m_cfg_stack_size,
        handle,
        [this](){ g_bbt_profiler->OnEvent_DoneCoroutine(); },
        g_bbt_coroutine_config->m_cfg_stack_protect);

    g_bbt_profiler->OnEvent_RegistCoroutine();
#else
    auto coroutine_sptr = Coroutine::Create(
        g_bbt_coroutine_config->m_cfg_stack_size,
        handle,
        g_bbt_coroutine_config->m_cfg_stack_protect);
#endif
    /* 尝试先找个Processer放进执行队列，失败放入全局队列 */
    AssertWithInfo(_LoadBlance2Proc(CO_PRIORITY_NORMAL, coroutine_sptr), "this is impossible!");    
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
    AssertWithInfo(m_global_coroutine_queue[priority].enqueue(coroutine), "oom!");
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
    m_begin_timestamp = bbt::core::clock::now<>();
    auto prev_scan_timepoint = bbt::core::clock::now<>();

#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_StartScheudler();
#endif
    while(m_is_running)
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
    m_is_running = false;

    _DestoryProcessers();
    
    if (m_sche_thread != nullptr) {
        if (m_sche_thread->joinable())
            m_sche_thread->join();
        delete m_sche_thread;
    }

    m_sche_thread = nullptr;
    Coroutine::Ptr item = nullptr;
    for (auto && queue : m_global_coroutine_queue)
        while (queue.try_dequeue(item))
            item = nullptr;

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
    /* 停止所有执行processer */
    for (auto item : m_processer_map)
        item.second->Stop();
    m_processer_map.clear();
    m_load_blance_vec.clear();

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
#pragma optimize("", off)
    auto& _init_tls_helper = g_bbt_tls_helper;
    auto& _init_poller = g_bbt_poller;
    auto& _init_profile = g_bbt_profiler;
    auto& _init_stackpool = g_bbt_stackpoll;
#pragma optimize("", on)
}

} // namespace bbt::coroutine::detail
