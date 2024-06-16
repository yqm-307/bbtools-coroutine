#include <cmath>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

namespace bbt::coroutine::detail
{

Scheduler::UPtr& Scheduler::GetInstance()
{
    static UPtr _inst{nullptr};
    if (_inst == nullptr)
        _inst = UPtr(new Scheduler());
    
    return _inst;
}


CoroutineId Scheduler::RegistCoroutineTask(const CoroutineCallback& handle)
{
    auto coroutine_sptr = Coroutine::Create(
        m_cfg_stack_size,
        handle,
        [](){});

    m_global_coroutine_deque.PushTail(coroutine_sptr);
    return coroutine_sptr->GetId();
}

// void Scheduler::UnRegistCoroutineTask(CoroutineId coroutine_id)
// {

// }

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

    if (m_global_coroutine_deque.Empty())
        return;
    
    /* 如果没有足够的processer，创建 */
    if (m_cfg_static_thread && (m_processer_map.size() < m_cfg_static_thread_num))
    {
        int need_create_thread_num = m_cfg_static_thread_num - m_processer_map.size();
        for (int i = 0; i< need_create_thread_num; ++i)
        {
            auto processer_sptr = Processer::Create();
            m_processer_map.insert(std::make_pair(processer_sptr->GetProcesserId(), processer_sptr));
            processer_sptr->Start(true);
        }
    }

    /* 取出待分配task */
    int total_loadvalue = 0;
    int avg_loadvalue = 0;
    std::map<Processer::SPtr, int> loadvalue_map;
    std::vector<Coroutine::SPtr> wait_tasks;
    m_global_coroutine_deque.PopAll(wait_tasks);


    /* 根据负载分配task */
    for (auto&& item : m_processer_map)
    {
        int load_value = item.second->GetLoadValue();
        loadvalue_map.insert(std::make_pair(item.second, load_value));

        total_loadvalue += load_value;
    }

    avg_loadvalue = std::floor((total_loadvalue + wait_tasks.size()) / m_processer_map.size()) + 1;


    int cur_pushed_index = 0;
    for (auto&& item : m_processer_map)
    {
        auto& processer_sptr = item.second;
        int processer_have_coroutine_num = processer_sptr->GetLoadValue();
        if (processer_have_coroutine_num >= avg_loadvalue)
            break;
        
        int give_coroutine_num = avg_loadvalue - processer_have_coroutine_num;
        auto begin_itor = wait_tasks.begin() + cur_pushed_index;
        auto end_itor = begin_itor + give_coroutine_num;
        if ((wait_tasks.size() - cur_pushed_index) < avg_loadvalue)
            end_itor = wait_tasks.end();

        processer_sptr->AddCoroutineTaskRange(begin_itor, end_itor);
    }

}


void Scheduler::_FixTimingScan()
{
    _SampleSchuduleAlgorithm();
}

void Scheduler::_Run()
{
    // auto prev_scan_timepoint = bbt::clock::now<>();

    while(m_is_running)
    {
        _FixTimingScan();
        // prev_scan_timepoint = prev_scan_timepoint + bbt::clock::ms(m_cfg_scan_interval_ms);
        // std::this_thread::sleep_until(prev_scan_timepoint);
    }
}

void Scheduler::Start(bool background_thread)
{
    if (background_thread)
    {
        auto t = new std::thread([this](){ _Run(); });
        t->detach();
        return;
    }

    _Run();
}

void Scheduler::Stop()
{
    m_is_running = false;

    for (auto item : m_processer_map)
        item.second->Stop();
}
} // namespace bbt::coroutine::detail
