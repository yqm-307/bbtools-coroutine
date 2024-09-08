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
    m_id(_GenProcesserId())
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
    return GetExecutableNum();
}

int Processer::GetExecutableNum()
{
    return m_coroutine_queue.size_approx();
}


ProcesserId Processer::GetId()
{
    return m_id;
}

void Processer::AddCoroutineTask(Coroutine::SPtr coroutine)
{
    AssertWithInfo(m_coroutine_queue.enqueue(coroutine), "oom!");
}

void Processer::AddCoroutineTaskRange(std::vector<Coroutine::SPtr> works)
{
    for (auto&& it : works)
        AssertWithInfo(m_coroutine_queue.enqueue(it), "oom!");
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
    /**
     * Processer主循环逻辑
     * 
     * 尝试从全局队列或者本地队列取协程对象，如果取不到就挂起P
     * 如果取到就在本地线程处理协程执行；
     * 
     * 处理完本地的协程后，尝试取窃取其他P的协程，如果窃取不到
     * 就挂起当前线程。
     */
    while (m_is_running)
    {
        m_run_status = ProcesserStatus::PROC_RUNNING;
        while (true)
        {
            // 如果本地没有了，尝试去全局取
            if (m_coroutine_queue.size_approx() <= 0)
                _TryGetCoroutineFromGlobal();

            /* 全局队列取不到且本地队列也为空，就休眠 */
            if (!m_coroutine_queue.try_dequeue(m_running_coroutine) || m_running_coroutine == nullptr)
                break;

            AssertWithInfo(m_running_coroutine->GetStatus() != CO_RUNNING && m_running_coroutine->GetStatus() != CO_FINAL, "bad coroutine status!");

            // 执行前设置当前协程缓存
            m_running_coroutine_begin.exchange( bbt::clock::gettime_mono<>());
            m_co_swap_times++;
            m_running_coroutine->Resume();
            m_running_coroutine = nullptr;
        }

        if (g_scheduler->TryWorkSteal(shared_from_this()) <= 0)
        {
            auto begin = bbt::clock::now<bbt::clock::microseconds>();
            std::unique_lock<std::mutex> lock_uptr(m_run_cond_mutex);
            m_run_status = ProcesserStatus::PROC_SUSPEND;
            m_run_cond.wait_for(lock_uptr, bbt::clock::us(g_bbt_coroutine_config->m_cfg_processer_proc_interval_us));
            m_suspend_cost_times += std::chrono::duration_cast<decltype(m_suspend_cost_times)>(bbt::clock::now<bbt::clock::microseconds>() - begin);
        }
    }

    m_run_status = ProcesserStatus::PROC_EXIT;
}

void Processer::Stop()
{
    Coroutine::SPtr item = nullptr;

    do {
        m_is_running = false;
        std::this_thread::sleep_for(bbt::clock::milliseconds(50));
        m_run_cond.notify_one();
    } while (m_run_status != ProcesserStatus::PROC_EXIT);


    while (m_coroutine_queue.try_dequeue(item))
        item = nullptr;
}

size_t Processer::_TryGetCoroutineFromGlobal()
{
    std::vector<Coroutine::SPtr> vec;
    g_scheduler->GetGlobalCoroutine(vec, g_bbt_coroutine_config->m_cfg_processer_get_co_from_g_count);
    for (auto it = vec.begin(); it != vec.end(); ++it)
        AssertWithInfo(m_coroutine_queue.enqueue(*it), "oom!");

    return vec.size();
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

void Processer::Steal(std::vector<Coroutine::SPtr>& works)
{
    Coroutine::SPtr item = nullptr;
    auto size = m_coroutine_queue.size_approx();

    if (size <= 0) {
        return;
    }
    
    uint64_t prev_run = m_running_coroutine_begin.load();
    auto already_run_time = bbt::clock::gettime_mono() - prev_run;
    if (already_run_time < g_bbt_coroutine_config->m_cfg_processer_worksteal_timeout_ms) {
        return;
    }

    int steal_num = g_bbt_coroutine_config->m_cfg_processer_steal_once_min_task_num > size ? g_bbt_coroutine_config->m_cfg_processer_steal_once_min_task_num : size / 2;

    /* 尽量给 */
    for (int i = 0; i < steal_num; ++i) {
        if (!m_coroutine_queue.try_dequeue(item))
            break;

        works.push_back(item);
    }

#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_StealSucc(steal_num);
#endif
}

} // namespace bbt::coroutine::detail
