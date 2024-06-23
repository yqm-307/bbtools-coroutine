#include <cmath>
#include <bbt/base/Logger/DebugPrint.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>

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
    if (m_thread != nullptr) {
        m_thread->join();
        delete m_thread;
    }

    for (auto&& proc_thread : m_proc_threads) {
        proc_thread->join();
        delete proc_thread;
    }
}


CoroutineId Scheduler::RegistCoroutineTask(const CoroutineCallback& handle)
{
#ifdef BBT_COROUTINE_PROFILE
    auto coroutine_sptr = Coroutine::Create(
        g_bbt_coroutine_config->m_cfg_stack_size,
        [this](){ g_bbt_profiler->OnEvent_DoneCoroutine(); },
        handle);

    g_bbt_profiler->OnEvent_RegistCoroutine();
#else
    auto coroutine_sptr = Coroutine::Create(
        g_bbt_coroutine_config->m_cfg_stack_size,
        handle);
#endif

    m_global_coroutine_deque.PushTail(coroutine_sptr);
    return coroutine_sptr->GetId();
}

void Scheduler::OnActiveCoroutine(Coroutine::SPtr coroutine)
{
    ProcesserId co_bind_procid = coroutine->GetBindProcesserId();
    std::unique_lock<std::mutex> _(m_processer_map_mutex);
    auto it = m_processer_map.find(co_bind_procid);
    Assert(it != m_processer_map.end());
    it->second->AddActiveCoroutine(coroutine);
}

void Scheduler::_SampleSchuduleAlgorithm()
{
    /**
     * 简单调度算法
     * 
     * coroutine创建完毕，放入global队列中。
     * 
     * 调度器扫描时，若global队列有任务，则分配到
     * 各个Processer中
     */
    /* 唤醒没有执行完毕所有任务但是阻塞的processer */
    for (auto&& item : m_processer_map)
    {
        auto processer = item.second;
        if ((processer->GetExecutableNum() > 0) && (processer->GetStatus() == ProcesserStatus::PROC_SUSPEND))
            processer->Notify();
    }

    
    /* 如果没有足够的processer，创建 */
    if (g_bbt_coroutine_config->m_cfg_static_thread && (m_processer_map.size() < g_bbt_coroutine_config->m_cfg_static_thread_num))
    {
        int need_create_thread_num = g_bbt_coroutine_config->m_cfg_static_thread_num - m_processer_map.size();
        for (int i = 0; i< need_create_thread_num; ++i)
        {
            auto t = new std::thread([this](){
                auto processer = Processer::GetLocalProcesser();
                {
                    std::lock_guard<std::mutex> _(this->m_processer_map_mutex);
                    this->m_processer_map.insert(std::make_pair(processer->GetId(), processer));
                }
                this->m_down_latch.Down();
                processer->Start(false);
            });
            m_proc_threads.push_back(t);
        }

        m_down_latch.Wait();
    }

    if (m_global_coroutine_deque.Empty())
        return;

    /* 取出待分配task */
    int total_coroutine_count = 0;
    int avg_coroutine_count = 0;
    std::map<Processer::SPtr, int> loadvalue_map;
    std::vector<Coroutine::SPtr> wait_tasks;
    m_global_coroutine_deque.PopAll(wait_tasks);


    /* 根据负载分配task */
    for (auto&& item : m_processer_map)
    {
        int load_value = static_cast<int>(item.second->GetLoadValue());
        loadvalue_map.insert(std::make_pair(item.second, load_value));
        total_coroutine_count += load_value;
    }

    avg_coroutine_count = std::floor((total_coroutine_count + wait_tasks.size()) / m_processer_map.size()) + 1;


    int cur_pushed_index = 0;
    for (auto&& item : m_processer_map)
    {
        auto& processer_sptr = item.second;
        int processer_have_coroutine_num = processer_sptr->GetLoadValue();
        if (processer_have_coroutine_num >= avg_coroutine_count)
            break;
        
        int give_coroutine_num = avg_coroutine_count - processer_have_coroutine_num;
        auto begin_itor = wait_tasks.begin() + cur_pushed_index;
        auto end_itor = begin_itor + give_coroutine_num;
        if ((wait_tasks.size() - cur_pushed_index) < avg_coroutine_count)
        {
            end_itor = wait_tasks.end();
            cur_pushed_index += wait_tasks.size() - cur_pushed_index;
        }
        else
        {
            cur_pushed_index += give_coroutine_num;
        }
        processer_sptr->AddCoroutineTaskRange(begin_itor, end_itor);
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
        } while(trigger_event > 0);
        prev_scan_timepoint = prev_scan_timepoint + bbt::clock::ms(g_bbt_coroutine_config->m_cfg_scan_interval_ms);
        std::this_thread::sleep_until(prev_scan_timepoint);
    }
}

void Scheduler::Start(bool background_thread)
{
    if (background_thread)
    {
        m_thread = new std::thread([this](){ _Run(); });
        return;
    }

    _Run();
}

void Scheduler::Stop()
{
    m_is_running = false;

    for (auto item : m_processer_map)
        item.second->Stop();
    
    m_run_status = ScheudlerStatus::SCHE_EXIT;
}

} // namespace bbt::coroutine::detail
