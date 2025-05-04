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

size_t Processer::GetLoadValue()
{
    return GetExecutableNum();
}

size_t Processer::GetExecutableNum()
{
    size_t total_size = 0;
    for (auto&& it : m_coroutine_queue)
    {
        if (it.size_approx() > 0)
            total_size += it.size_approx();
    }

    return total_size;
}


ProcesserId Processer::GetId()
{
    return m_id;
}

void Processer::AddCoroutineTask(CoroutinePriority priority, Coroutine::SPtr coroutine)
{
    AssertWithInfo(coroutine != nullptr, "coroutine is nullptr!");
    AssertWithInfo(m_coroutine_queue[priority].enqueue(coroutine), "oom!");
    if (m_run_status == ProcesserStatus::PROC_SUSPEND)
        m_run_cond.notify_one();
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

        // 如果本地没有了，尝试去全局取
        if (GetExecutableNum() <= 0)
            _TryGetCoroutineFromGlobal();

        // 对各个优先级任务进行执行，根据优先级自高到低依次执行
        for (auto&& p : {CO_PRIORITY_CRITICAL, CO_PRIORITY_HIGH, CO_PRIORITY_NORMAL, CO_PRIORITY_LOW})
        {
            while (true)
            {
                /* 如果取不到或者取到空的，就退出循环 */
                if (!m_coroutine_queue[p].try_dequeue(m_running_coroutine) || m_running_coroutine == nullptr)
                    break;

                AssertWithInfo(m_running_coroutine->GetStatus() != CO_RUNNING && m_running_coroutine->GetStatus() != CO_FINAL, "bad coroutine status!");

                // 执行前设置当前协程缓存
                m_running_coroutine_begin.exchange( bbt::core::clock::gettime_mono<>());
#ifdef BBT_COROUTINE_PROFILE
            m_co_swap_times++;
#endif
                m_running_coroutine->Resume();
                m_running_coroutine = nullptr;
            }
        }

        if (g_scheduler->TryWorkSteal(shared_from_this()) <= 0)
        {
#ifdef BBT_COROUTINE_PROFILE
            auto begin = bbt::core::clock::now<bbt::core::clock::microseconds>();
#endif
            std::unique_lock<std::mutex> lock_uptr(m_run_cond_mutex);
            m_run_status = ProcesserStatus::PROC_SUSPEND;
            m_run_cond.wait_for(lock_uptr, bbt::core::clock::us(g_bbt_coroutine_config->m_cfg_processer_proc_interval_us));
#ifdef BBT_COROUTINE_PROFILE
            m_suspend_cost_times += std::chrono::duration_cast<decltype(m_suspend_cost_times)>(bbt::core::clock::now<bbt::core::clock::microseconds>() - begin);
#endif
        }
    }

    m_run_status = ProcesserStatus::PROC_EXIT;
}

void Processer::Stop()
{
    Coroutine::SPtr item = nullptr;

    do {
        m_is_running = false;
        std::this_thread::sleep_for(bbt::core::clock::milliseconds(50));
        m_run_cond.notify_one();
    } while (m_run_status != ProcesserStatus::PROC_EXIT);


    /* 释放所有协程 */
    for (auto && it : m_coroutine_queue)
        while (it.try_dequeue(item))
            item = nullptr;

}

size_t Processer::_TryGetCoroutineFromGlobal()
{
    /* 希望获取的总数 */ 
    auto expected_size = g_bbt_coroutine_config->m_cfg_processer_get_co_from_g_count;
    /* 已经处理的数量 */
    size_t already_count = 0;

    /* 每个队列都尝试去全局队列中取一下 */
    for (auto && priority : { CO_PRIORITY_CRITICAL, CO_PRIORITY_HIGH, CO_PRIORITY_NORMAL, CO_PRIORITY_LOW})
    {
        already_count += g_scheduler->GetCoroutineFromGlobal(priority, m_coroutine_queue[priority], expected_size - already_count);
        if (already_count >= expected_size)
            break;
    }

    return already_count;
}

Coroutine::SPtr Processer::GetCurrentCoroutine()
{
    return m_running_coroutine;
}

uint64_t Processer::GetContextSwapTimes()
{
#ifdef BBT_COROUTINE_PROFILE
    return m_co_swap_times;
#else
    return 0;
#endif
}

uint64_t Processer::GetSuspendCostTime()
{
#ifdef BBT_COROUTINE_PROFILE
    return m_suspend_cost_times.count();
#else
    return 0;
#endif
}

size_t Processer::Steal(Processer::SPtr thief)
{
    Coroutine::SPtr item = nullptr;
    size_t steal_num = 0;
    size_t expect_size = 0;

    /* 如果没有任务就返回 */
    size_t size = GetExecutableNum();
    if (size <= 0)
        return steal_num;
    
    /* 运行时间过久的才需要偷 */
    uint64_t prev_run = m_running_coroutine_begin.load();
    auto already_run_time = bbt::core::clock::gettime_mono() - prev_run;
    if (already_run_time < g_bbt_coroutine_config->m_cfg_processer_worksteal_timeout_ms) {
        return steal_num;
    }

    /* 计算下需要偷多少个 */
    expect_size = g_bbt_coroutine_config->m_cfg_processer_steal_once_min_task_num > size ? g_bbt_coroutine_config->m_cfg_processer_steal_once_min_task_num : size / 2;

    for (auto && p : {CO_PRIORITY_CRITICAL, CO_PRIORITY_HIGH, CO_PRIORITY_NORMAL, CO_PRIORITY_LOW})
    {
        if (steal_num >= expect_size)
            break;

        size_t count = 0;
        for (size_t i = 0; i < (expect_size - steal_num); ++i)
        {
            if (!m_coroutine_queue[p].try_dequeue(item))
                break;

            AssertWithInfo(thief->m_coroutine_queue[p].enqueue(item), "oom!");
            item = nullptr;
            ++count;
        }

        steal_num += count;
    }

#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_StealSucc(steal_num);
#endif

    return steal_num;
}

} // namespace bbt::coroutine::detail
