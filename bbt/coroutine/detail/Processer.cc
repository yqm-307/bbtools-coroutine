#include <algorithm>
#include <array>
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
    /**
     * Processer绑定一个线程，且每个线程只能有一个Processer。
     */
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
        total_size += it.size_approx();
    }

    return total_size;
}


ProcesserId Processer::GetId()
{
    return m_id;
}

void Processer::AddCoroutineTask(CoroutinePriority priority, Coroutine::Ptr coroutine)
{
    AssertWithInfo(coroutine != nullptr, "coroutine is nullptr!");
    AssertWithInfo(m_coroutine_queue[priority].enqueue(coroutine), "oom!");

    bool need_notify = true;
    if (m_run_status == ProcesserStatus::PROC_SUSPEND && m_run_cond_notify.compare_exchange_strong(need_notify, false))        
        m_run_cond.notify_one();
}

void Processer::_Init()
{
    m_run_status.store(ProcesserStatus::PROC_DEFAULT, std::memory_order_release);
    m_is_shutdown.store(false, std::memory_order_release);
    m_is_running.store(true, std::memory_order_release);
    m_run_cond_notify.store(true, std::memory_order_release);
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
     * 1. 尝试从全局队列或者本地队列取协程对象
     *      - 若取不到，就挂起Processer线程
     *      - 若取得到，本地线程处理协程执行
     * 
     * 2. 处理完本地的协程后，尝试取窃取其他P的协程
     *      - 如果窃取不到就挂起当前线程。
     *      - 如果窃取到，就执行窃取到的协程
     * 
     * 3. 重复1、2步骤，直到Processer被Stop()方法调用
     * 
     * XXX 这里也许可以优化的点：
     *      - 是否在空闲的时候降低调度频率？
     */
    static constexpr std::array<uint64_t, CO_PRIORITY_COUNT> kPriorityRuntimeBudgetUs = {
        50,   // LOW
        150,  // NORMAL
        200,  // HIGH
        600,  // CRITICAL
    };

    auto priority_runtime_budget_us = kPriorityRuntimeBudgetUs;

    auto reset_priority_budget_if_needed = [&]() {
        bool has_runnable_queue = false;
        bool has_runnable_queue_with_budget = false;
        for (auto&& p : {CO_PRIORITY_CRITICAL,
                         CO_PRIORITY_HIGH,
                         CO_PRIORITY_NORMAL,
                         CO_PRIORITY_LOW})
        {
            if (m_coroutine_queue[p].size_approx() == 0)
                continue;

            has_runnable_queue = true;
            if (priority_runtime_budget_us[p] > 0)
                has_runnable_queue_with_budget = true;
        }

        // 空队列不参与预算耗尽判定；仅当所有非空队列都耗尽预算
        // （或当前没有本地 runnable 队列）时开启下一公平轮次。
        if (!has_runnable_queue || !has_runnable_queue_with_budget)
            priority_runtime_budget_us = kPriorityRuntimeBudgetUs;
    };

    while (m_is_running.load(std::memory_order_acquire))
    {
        m_run_status = ProcesserStatus::PROC_RUNNING;

        // 如果本地没有了，尝试去全局取
        if (GetExecutableNum() <= 0)
            _TryGetCoroutineFromGlobal();

        // 对各个优先级任务进行执行，按运行时间预算（微秒）加权轮转
        bool any_dequeued = false;
        for (auto&& p : {CO_PRIORITY_CRITICAL, CO_PRIORITY_HIGH, CO_PRIORITY_NORMAL, CO_PRIORITY_LOW})
        {
            if (m_is_shutdown.load(std::memory_order_acquire))
            {
                while (m_coroutine_queue[p].try_dequeue(m_running_coroutine))
                {
                    delete m_running_coroutine;
                    m_running_coroutine = nullptr;
                }
                continue;
            }

            while (priority_runtime_budget_us[p] > 0)
            {
                /* 如果取不到或者取到空的，就退出循环 */
                if (!m_coroutine_queue[p].try_dequeue(m_running_coroutine) || m_running_coroutine == nullptr)
                {
                    /* 本地该优先级队列已空但预算未耗尽：
                     * 按优先级从全局队列补充任务，避免 CRITICAL 全局积压
                     * 使 NORMAL 等低优先级协程长期无法被取回本地执行。
                     * 一次只搬 1 个（最小必要粒度），避免每轮按 16 个批量搬运
                     * 放大全局队列竞争；单轮搬运总量仍由预算循环门控 */
                    if (g_scheduler->GetCoroutineFromGlobal(p, m_coroutine_queue[p], 1) <= 0)
                        break;
                    continue;
                }
                any_dequeued = true;

                /* 强制关闭模式：跳过协程执行，直接回收（不扣预算） */
                if (m_is_shutdown.load(std::memory_order_acquire)) {
                    delete m_running_coroutine;
                    m_running_coroutine = nullptr;
                    continue;
                }

                AssertWithInfo(m_running_coroutine->GetStatus() != CO_RUNNING && m_running_coroutine->GetStatus() != CO_FINAL, "bad coroutine status!");

                // 执行前设置当前协程缓存（预算单位为微秒，计时必须显式 us）
                m_running_coroutine_begin.exchange(bbt::core::clock::gettime_mono<bbt::core::clock::us>());
#ifdef BBT_COROUTINE_PROFILE
                m_co_swap_times++;
#endif
                m_running_coroutine->Resume();
                // MLFQ: 记录运行时长（微秒），供后续降级判断
                m_running_coroutine->SetLastRunTimeUs(
                    bbt::core::clock::gettime_mono<bbt::core::clock::us>() - m_running_coroutine_begin.load());

                // 按本次实际运行时长扣减优先级预算，单次至少扣 1 微秒
                const auto charged_us = std::max<uint64_t>(
                    1, m_running_coroutine->GetLastRunTimeUs());
                priority_runtime_budget_us[p] =
                    charged_us >= priority_runtime_budget_us[p]
                        ? 0
                        : priority_runtime_budget_us[p] - charged_us;

                const auto disposition = m_running_coroutine->CommitYield();
                if (disposition == CoroutineYieldDisposition::READY) {
                    g_scheduler->OnActiveCoroutine(CO_PRIORITY_NORMAL, m_running_coroutine);
                }
                else if (disposition == CoroutineYieldDisposition::FINAL) {
                    delete m_running_coroutine;
                }
                m_running_coroutine = nullptr;
            }
        }

        // 检测 size_approx 假阳性：认为有任务但取不到任何协程
        if (!any_dequeued && GetExecutableNum() > 0) {
#ifdef BBT_COROUTINE_PROFILE
            m_stall_loop_count++;
#endif
        }

        // work-conserving：只按非空队列判断预算耗尽，避免空队列的
        // 未用预算阻止公平轮次重置；不提前 continue，保留空转休眠路径。
        reset_priority_budget_if_needed();

        // 每 CHECK_INTERVAL 轮无条件检查全局队列
        // spin worker 会让本地队列 size_approx 始终非零
        if (++m_global_check_counter >= kGlobalCheckInterval) {
            m_global_check_counter = 0;
            _TryGetCoroutineFromGlobal();
        }

        m_run_status = ProcesserStatus::PROC_SUSPEND;
        auto steal_num = g_scheduler->TryWorkSteal(shared_from_this());
#ifdef BBT_COROUTINE_PROFILE
        m_steal_succ_times += steal_num;
#endif

        if (steal_num <= 0)
        {
#ifdef BBT_COROUTINE_PROFILE
            auto begin = bbt::core::clock::now<bbt::core::clock::microseconds>();
#endif
            std::unique_lock<std::mutex> lock_uptr(m_run_cond_mutex);
            m_run_cond.wait_for(lock_uptr,
                                bbt::core::clock::us(g_bbt_coroutine_config->m_cfg_processer_proc_interval_us),
                                [this] {
                                    return !m_is_running.load(std::memory_order_acquire) ||
                                        !m_run_cond_notify.load(std::memory_order_acquire);
                                });
            m_run_cond_notify.store(true, std::memory_order_release);
#ifdef BBT_COROUTINE_PROFILE
            m_suspend_cost_times += std::chrono::duration_cast<decltype(m_suspend_cost_times)>(bbt::core::clock::now<bbt::core::clock::microseconds>() - begin);
#endif
        }
    }

    m_run_status = ProcesserStatus::PROC_EXIT;
}

void Processer::Stop()
{
    Coroutine::Ptr item = nullptr;

    m_is_running.store(false, std::memory_order_release);
    m_run_cond.notify_all();

    /* 等待 _Run 循环退出（最多 5 秒） */
    constexpr int kMaxRetries = 100;
    for (int retry = 0; retry < kMaxRetries; ++retry) {
        if (m_run_status == ProcesserStatus::PROC_EXIT)
            break;
        m_run_cond.notify_one();
        std::this_thread::sleep_for(bbt::core::clock::milliseconds(50));
    }

    /* 超时强杀：直接回收协程 */
    if (m_run_status != ProcesserStatus::PROC_EXIT) {
        // 设置 shutdown 标志，_Run 循环检测到此标志时跳过协程执行
        m_is_shutdown.store(true, std::memory_order_release);
        m_run_cond.notify_all();
        std::this_thread::sleep_for(bbt::core::clock::milliseconds(100));
    }

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

Coroutine::Ptr Processer::GetCurrentCoroutine()
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

uint64_t Processer::GetStealSuccTimes()
{
#ifdef BBT_COROUTINE_PROFILE
    return m_steal_succ_times;
#else
    return 0;
#endif
}

uint64_t Processer::GetStealCount()
{
#ifdef BBT_COROUTINE_PROFILE
    return m_steal_count;
#else
    return 0;
#endif
}

uint64_t Processer::GetStallLoopCount()
{
#ifdef BBT_COROUTINE_PROFILE
    return m_stall_loop_count;
#else
    return 0;
#endif
}

size_t Processer::Steal(Processer::SPtr thief)
{
    Coroutine::Ptr item = nullptr;
    size_t steal_num = 0;
    size_t expect_size = 0;

    /* 如果没有任务就返回 */
    size_t size = GetExecutableNum();
    if (size <= 0)
        return steal_num;
    
    /* 运行时间过久的才需要偷。
     * m_running_coroutine_begin 与预算同为微秒（gettime_mono<us>），
     * elapsed 必须用 us 计时；与 ms 配置比较前先换算为 us，避免 ms/us 混算 */
    uint64_t prev_run = m_running_coroutine_begin.load();
    auto already_run_time_us =
        bbt::core::clock::gettime_mono<bbt::core::clock::us>() - prev_run;
    if (already_run_time_us <
        static_cast<uint64_t>(g_bbt_coroutine_config->m_cfg_processer_worksteal_timeout_ms) * 1000) {
        return steal_num;
    }

    /* 计算下需要偷多少个 */
    expect_size = g_bbt_coroutine_config->m_cfg_processer_steal_once_min_task_num > size / 2 ? g_bbt_coroutine_config->m_cfg_processer_steal_once_min_task_num : size / 2;

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
    m_steal_count += steal_num;
#endif

    return steal_num;
}

} // namespace bbt::coroutine::detail
