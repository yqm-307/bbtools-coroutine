#include <bbt/base/assert/Assert.hpp>
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
    m_scheduler_begin_timestamp = bbt::clock::now<>();
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


void Profiler::ProfileInfo(std::string& info)
{
    info.clear();
    info += "================== Scheduler ProfileInfo ==================\n";
    info += "已运行时间(ms)：" + std::to_string((bbt::clock::now<>() - m_scheduler_begin_timestamp).count()) + '\n';
    info += "完成协程数：" + std::to_string(m_total_done_co_count.load())  + '\n';
    info += "注册协程数：" + std::to_string(m_total_regist_co_count.load()) + '\n';
    info += "执行速率(n/ms)："   + std::to_string(m_total_done_co_count.load() / (bbt::clock::now<>() - m_scheduler_begin_timestamp).count()) + '\n';
    info += "未释放协程数：" + std::to_string(m_create_coroutine_count.load() - m_destory_coroutine_count.load()) + '\n';
    info += "StackPool大小：" + std::to_string(g_bbt_stackpoll->AllocSize()) + '\n';
    info += "StackPool Rtts：" + std::to_string(g_bbt_stackpoll->GetRtts()) + '\n';
    for (auto&& processer : m_processer_map)
    {
        info += "\tPID："                    + std::to_string(processer.second->GetId()) +
                "\t本地队列任务数："          + std::to_string(processer.second->GetExecutableNum()) + 
                "\t上下文切换次数："          + std::to_string(processer.second->GetContextSwapTimes()) +
                "\t挂起时间(ms)："            + std::to_string(processer.second->GetSuspendCostTime() / 1000) +
                '\n';
    }
}

} // namespace bbt::coroutine::detail
