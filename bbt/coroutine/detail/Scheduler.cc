#include <cmath>
#include <bbt/base/Logger/DebugPrint.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>

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
    m_down_latch(g_bbt_coroutine_config->m_cfg_static_thread_num),
    m_global_coroutine_deque(true)
{
}

Scheduler::~Scheduler()
{
}

void Scheduler::_Init()
{
    m_thread = nullptr;
    m_is_running = true;
    m_run_status = SCHE_DEFAULT;
    m_regist_coroutine_count = 0;
    m_down_latch.Reset(g_bbt_coroutine_config->m_cfg_static_thread_num);
}

CoroutineId Scheduler::RegistCoroutineTask(const CoroutineCallback& handle)
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
    if (!_LoadBlance2Proc(coroutine_sptr)) {
        m_global_coroutine_deque.PushTail(coroutine_sptr);
    }
    
    return coroutine_sptr->GetId();
}

void Scheduler::OnActiveCoroutine(Coroutine::SPtr coroutine)
{
    Assert(!m_global_coroutine_deque.Exist(coroutine));
    m_global_coroutine_deque.PushTail(coroutine);
}

void Scheduler::_SampleSchuduleAlgorithm()
{
    if (!m_global_coroutine_deque.Empty())
    {
        for (auto&& item : m_processer_map)
        {
            auto processer = item.second;
            processer->Notify();
        }
    }
}


void Scheduler::_FixTimingScan()
{
    m_run_status = ScheudlerStatus::SCHE_RUNNING;
    _SampleSchuduleAlgorithm();
    m_run_status = ScheudlerStatus::SCHE_SUSPEND;
}

void Scheduler::_Run()
{
    m_begin_timestamp = bbt::clock::now<>();
    auto prev_scan_timepoint = bbt::clock::now<>();
    auto prev_profile_timepoint = bbt::clock::now<>();
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_StartScheudler();
#endif
    while(m_is_running)
    {
        
#ifdef BBT_COROUTINE_PROFILE
        if (g_bbt_coroutine_config->m_cfg_profile_printf_ms > 0 &&
            bbt::clock::is_expired<bbt::clock::milliseconds>((prev_profile_timepoint + bbt::clock::milliseconds(g_bbt_coroutine_config->m_cfg_profile_printf_ms))))
        {
            std::string info;
            g_bbt_profiler->ProfileInfo(info);
            bbt::log::DebugPrint(info.c_str());
            prev_profile_timepoint = bbt::clock::now<>();
        }
#endif

        int trigger_event = 0;
        do {
            trigger_event = g_bbt_poller->PollOnce();
            _FixTimingScan();
            g_bbt_stackpoll->OnUpdate();
        } while(trigger_event > 0);
        prev_scan_timepoint = prev_scan_timepoint + bbt::clock::ms(g_bbt_coroutine_config->m_cfg_scan_interval_ms);
        std::this_thread::sleep_until(prev_scan_timepoint);
    }
}

void Scheduler::Start(bool background_thread)
{
    _Init();
    bbt::thread::CountDownLatch latch{1};
    if (background_thread)
    {
        Assert(m_thread == nullptr);
        m_thread = new std::thread([this, &latch](){
            _CreateProcessers();
            latch.Down();
            _Run();
        });

        latch.Wait();
    } else {
        _CreateProcessers();
        _Run();
    }

}

void Scheduler::Stop()
{
    m_is_running = false;

    _DestoryProcessers();
    
    if (m_thread != nullptr) {
        if (m_thread->joinable())
            m_thread->join();
        delete m_thread;
    }

    m_thread = nullptr;
    m_global_coroutine_deque.Clear();
    m_run_status = ScheudlerStatus::SCHE_EXIT;
#ifdef BBT_COROUTINE_PROFILE
    std::string profileinfo;
    g_bbt_profiler->ProfileInfo(profileinfo);
    bbt::log::DebugPrint(profileinfo.c_str());
#endif

}

size_t Scheduler::GetGlobalCoroutine(std::vector<Coroutine::SPtr>& coroutines, size_t size)
{
    coroutines.clear();
    m_global_coroutine_deque.PopNTail(coroutines, size);
    return coroutines.size();
}

void Scheduler::_CreateProcessers()
{
    int need_create_thread_num = g_bbt_coroutine_config->m_cfg_static_thread_num - m_processer_map.size();
    for (int i = 0; i < need_create_thread_num; ++i)
    {
        auto t = new std::thread([this](){
            g_bbt_tls_helper->SetEnableUseCo(true);
            auto processer = g_bbt_tls_processer;
            {
                std::lock_guard<std::mutex> _(this->m_processer_map_mutex);
                this->m_processer_map.insert(std::make_pair(processer->GetId(), processer));
                m_load_blance_vec.push_back(processer);
            }
            this->m_down_latch.Down();
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

bool Scheduler::_LoadBlance2Proc(Coroutine::SPtr co)
{
    uint32_t index = m_load_idx;
    m_load_idx++;

    index %= g_bbt_coroutine_config->m_cfg_static_thread_num;
    auto proc = m_load_blance_vec[index];
    if (proc->GetStatus() != ProcesserStatus::PROC_SUSPEND)
        return false;
    
    proc->AddCoroutineTask(co);    

    return true;
}

} // namespace bbt::coroutine::detail
