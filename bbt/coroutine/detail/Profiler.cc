#include <bbt/core/util/Assert.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>

namespace bbt::coroutine::detail
{

Profiler::UPtr& Profiler::GetInstance()
{
    static UPtr _inst = nullptr;
    if (_inst == nullptr)
        _inst = UPtr(new Profiler());

    return _inst;
}

void Profiler::OnEvent_RegistCoroutine()
{
    m_total_regist_co_count++;
}

void Profiler::OnEvent_DoneCoroutine()
{
    m_total_done_co_count++;
}


void Profiler::OnEvent_CreateCoroutine()
{
    m_create_coroutine_count++;
}

void Profiler::OnEvent_DestoryCoroutine()
{
    m_destory_coroutine_count++;
}

void Profiler::OnEvent_StartScheudler()
{
    m_scheduler_begin_timestamp = bbt::core::clock::now<>();
}

void Profiler::OnEvent_RegistCoPollEvent()
{
    m_regist_event_count++;
}

void Profiler::OnEvent_TriggerCoPollEvent()
{
    m_trigger_event_count++;
}

void Profiler::OnEvent_StartProcesser(Processer::SPtr proc)
{
    std::unique_lock<std::mutex> _(m_processer_map_mutex);
    ProcesserId id = proc->GetId();
    auto it = m_processer_map.find(id);
    Assert(it == m_processer_map.end());
    m_processer_map.insert(std::make_pair(id, proc));
}

void Profiler::OnEvent_StopPorcesser(Processer::SPtr proc)
{
    std::unique_lock<std::mutex> _(m_processer_map_mutex);
    ProcesserId id = proc->GetId();
    auto it = m_processer_map.find(id);
    Assert(it != m_processer_map.end());
    m_processer_map.erase(it);
}

void Profiler::OnEvent_StealSucc(int num)
{
    Assert(num >= 0);
    m_total_steal_count.fetch_add(num);
}

void Profiler::OnEvent_StackRelease(int num)
{
    m_stack_release_count += num;
}

void Profiler::OnEvent_StackAlloc()
{
    m_stack_alloc_count++;
}

void Profiler::ProfileInfo(std::string& info)
{
    info.clear();
    info += "================== Scheduler ProfileInfo ==================\n";
    info += "已运行时间(ms)："  + std::to_string((bbt::core::clock::now<>() - m_scheduler_begin_timestamp).count()) + '\n';
    info += "完成协程数："      + std::to_string(m_total_done_co_count.load())  + '\n';
    info += "注册协程数："      + std::to_string(m_total_regist_co_count.load()) + '\n';
    info += "执行速率(n/ms)："  + std::to_string(m_total_done_co_count.load() / (bbt::core::clock::now<>() - m_scheduler_begin_timestamp).count()) + '\n';
    info += "未释放协程数："    + std::to_string(m_create_coroutine_count.load() - m_destory_coroutine_count.load()) + '\n';
    info += "Steal数量："       + std::to_string(m_total_steal_count.load()) + '\n';
    info += "StackPool大小："   + std::to_string(g_bbt_stackpoll->AllocSize()) + '\n';
    info += "StackPool Rtts："  + std::to_string(g_bbt_stackpoll->GetCoAvgCount()) + '\n';
    info += "CoEvent 注册数量："  + std::to_string(m_regist_event_count.load()) + '\n';
    info += "CoEvent 触发数量："  + std::to_string(m_trigger_event_count.load()) + '\n';
    info += "StackPool 指标, alloc_count=" + std::to_string(m_stack_alloc_count.load()) + "\trelease_count=" + std::to_string(m_stack_release_count.load()) + '\n';
    for (auto&& processer : m_processer_map)
    {
        info += "\tPID："                    + std::to_string(processer.second->GetId()) +
                "\t本地队列任务数："          + std::to_string(processer.second->GetExecutableNum()) + 
                "\t上下文切换次数："          + std::to_string(processer.second->GetContextSwapTimes()) +
                "\t挂起时间(ms)："            + std::to_string(processer.second->GetSuspendCostTime() / 1000) +
                "\tSteal数量(偷/被偷)："           + std::to_string(processer.second->GetStealSuccTimes()) + "/" + std::to_string(processer.second->GetStealCount()) +
                '\n';
    }
}

} // namespace bbt::coroutine::detail
