#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

namespace bbt::coroutine::detail
{

CoroutineId Scheduler::RegistCoroutineTask(const CoroutineCallback& handle)
{
    auto coroutine_sptr = Coroutine::Create(
        m_cfg_stack_size,
        handle,
        [this]() { _OnCoroutineFinal(); });
    
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

    int 
    if (m_global_coroutine_deque.Empty())
        return;
    
    
    for (auto&& processer : m_processer_map)
    {

    }

}


void Scheduler::_FixTimingScan()
{

    

}


void Scheduler::Start()
{
    auto prev_scan_timepoint = bbt::clock::now<>();

    while(m_is_running)
    {
        _FixTimingScan();
        prev_scan_timepoint = prev_scan_timepoint + bbt::clock::ms(m_cfg_scan_interval_ms);
        std::this_thread::sleep_until(prev_scan_timepoint);
    }
}

void Scheduler::Stop()
{
    m_is_running = false;
}
} // namespace bbt::coroutine::detail
