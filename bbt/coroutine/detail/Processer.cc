#include <atomic>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

namespace bbt::coroutine::detail
{

Processer::SPtr Processer::Create()
{
    return std::make_shared<Processer>();
}

Processer::SPtr Processer::GetLocalProcesser()
{
    static thread_local Processer::SPtr tl_processer = nullptr;
    if (tl_processer == nullptr) {
        tl_processer = Create();
#ifdef BBT_COROUTINE_PROFILE
        g_bbt_profiler->OnEvent_StartProcesser(tl_processer);
#endif
    }

    return tl_processer;
}

ProcesserId Processer::_GenProcesserId()
{
    static std::atomic_uint64_t _generate_id{BBT_COROUTINE_INVALID_PROCESSER_ID};
    return (++_generate_id);
}

Processer::Processer():
    m_id(_GenProcesserId()),
    m_coroutine_queue(true)
{
    m_run_status = ProcesserStatus::PROC_SUSPEND;
}

Processer::~Processer()
{
}

ProcesserStatus Processer::GetStatus()
{
    return m_run_status;
}

int Processer::GetLoadValue()
{
    Assert(m_coroutine_queue.Size() >= 0);
    return m_coroutine_queue.Size();
}

int Processer::GetExecutableNum()
{
    return m_coroutine_queue.Size();
}


ProcesserId Processer::GetId()
{
    return m_id;
}

void Processer::AddCoroutineTask(Coroutine::SPtr coroutine)
{
    m_coroutine_queue.PushTail(coroutine);
    _OnAddCorotinue();
}

void Processer::AddCoroutineTaskRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end)
{
    m_coroutine_queue.PushTailRange(begin, end);
    _OnAddCorotinue();
}

void Processer::_Init()
{
    m_run_status = ProcesserStatus::PROC_DEFAULT;
    m_is_running = true;
    m_running_coroutine = nullptr;
}

void Processer::Start(bool background_thread)
{
    _Init();
    if (background_thread)
    {
        auto t = new std::thread([this](){_Run();});
        t->detach();
        return;
    }

    _Run();
}

void Processer::_Run()
{
    while (m_is_running)
    {
        m_run_status = ProcesserStatus::PROC_RUNNING;
        // 执行本地任务
        while (m_coroutine_queue.Size() > 0 || _TryGetCoroutineFromGlobal() > 0 )
        {
            std::vector<Coroutine::SPtr> pending_coroutines;
            // m_coroutine_queue.PopAll(pending_coroutines);
            m_coroutine_queue.PopNHead(pending_coroutines, g_bbt_coroutine_config->m_cfg_processer_do_task_once_task_num);

            for (auto&& coroutine : pending_coroutines) {
                if (coroutine->GetStatus() == CO_RUNNING || coroutine->GetStatus() == CO_FINAL)
                    continue;

                // 执行前设置当前协程
                m_running_coroutine = coroutine;
                m_running_coroutine_begin.exchange( bbt::clock::gettime_mono<>());
                AssertWithInfo(m_running_coroutine != nullptr, "maybe coroutine queue has bug!");
                AssertWithInfo(m_running_coroutine->GetStatus() != CoroutineStatus::CO_RUNNING, "error, try to resume a already running coroutine!");
                m_co_swap_times++;
                m_running_coroutine->Resume();
            }
        }

        // 尝试窃取其他Processer任务，失败挂起
        if (g_scheduler->TryWorkSteal(shared_from_this()) <= 0)
        {
            auto begin = bbt::clock::now<bbt::clock::microseconds>();
            std::unique_lock<std::mutex> lock_uptr(m_run_cond_mutex);
            m_run_status = ProcesserStatus::PROC_SUSPEND;
            m_run_cond.wait_for(lock_uptr, bbt::clock::milliseconds(1));
            m_suspend_cost_times += std::chrono::duration_cast<decltype(m_suspend_cost_times)>(bbt::clock::now<bbt::clock::microseconds>() - begin);
        }
    }

    m_run_status = ProcesserStatus::PROC_EXIT;
}

void Processer::Stop()
{
    do {
        m_is_running = false;
        std::this_thread::sleep_for(bbt::clock::milliseconds(50));
        m_run_cond.notify_one();
    } while (m_run_status != ProcesserStatus::PROC_EXIT);
    m_coroutine_queue.Clear();
}

void Processer::_OnAddCorotinue()
{
    // if (!m_is_running || !(m_run_status == ProcesserStatus::PROC_Suspend))
    if (!m_is_running)
        return;

    m_run_cond.notify_one();
}

size_t Processer::_TryGetCoroutineFromGlobal()
{
    std::vector<Coroutine::SPtr> vec;
    g_scheduler->GetGlobalCoroutine(vec, g_bbt_coroutine_config->m_cfg_processer_get_co_from_g_count);
    m_coroutine_queue.PushTailRange(vec.begin(), vec.end());
    return vec.size();
}


void Processer::Notify()
{
    m_run_cond.notify_one();
}

Coroutine::SPtr Processer::GetCurrentCoroutine()
{
    return m_running_coroutine;
}

uint64_t Processer::GetContextSwapTimes()
{
    return m_co_swap_times;
}

uint64_t Processer::GetSuspendCostTime()
{
    return m_suspend_cost_times.count();
}

void Processer::_Steal(std::vector<Coroutine::SPtr>& works)
{
    auto size = m_coroutine_queue.Size();
    if (size <= 0)
        return;
    
    uint64_t prev_run = m_running_coroutine_begin.load();
    auto already_run_time = bbt::clock::gettime_mono() - prev_run;
    if (already_run_time < g_bbt_coroutine_config->m_cfg_processer_worksteal_timeout_ms)
        return;

    int steal_num = g_bbt_coroutine_config->m_cfg_processer_steal_once_min_task_num > size ? g_bbt_coroutine_config->m_cfg_processer_steal_once_min_task_num : size / 2;
    m_coroutine_queue.PopNTail(works, steal_num);

#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_StealSucc(steal_num);
#endif
}

} // namespace bbt::coroutine::detail
