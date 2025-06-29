#pragma once
#include <condition_variable>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/utils/lockfree/concurrentqueue.h>

namespace bbt::coroutine::detail
{

class Processer:
    public std::enable_shared_from_this<Processer>
{
public:
    friend class Scheduler;
    friend class Profiler;
    typedef std::shared_ptr<Processer> SPtr;

    static SPtr Create();
    /* 获取当前线程Processer */
    static SPtr GetLocalProcesser();

    explicit
    Processer();
    ~Processer();

    /* 获取processer当前状态 */
    ProcesserStatus                 GetStatus();
    /* 获取processer id */
    ProcesserId                     GetId();
    Coroutine::Ptr                 GetCurrentCoroutine();

protected:
    /* 非公开库内部接口 */
    void                            Start(bool background_thread = true);
    void                            Stop();
    size_t                          GetLoadValue();
    size_t                          GetExecutableNum(); /* 可执行协程数 */
    void                            AddCoroutineTask(CoroutinePriority priority, Coroutine::Ptr coroutine);

    uint64_t                        GetContextSwapTimes();  /* 协程上下文换出次数 */
    uint64_t                        GetSuspendCostTime();     /* 任务执行耗时，返回微秒 */
    uint64_t                        GetStealSuccTimes(); /* 窃取他人成功次数 */
    uint64_t                        GetStealCount(); /* 被窃取次数 */
    /**
     * @brief 从Processer中窃取任务（线程安全）
     * 
     * @param works 窃取的任务
     * @return 偷取任务数量
     */
    size_t                          Steal(Processer::SPtr proc); /* 偷取任务 */
protected:
    void                            _Init();
    static ProcesserId              _GenProcesserId();
    size_t                          _TryGetCoroutineFromGlobal();
    void                            _Run();
private:
    const ProcesserId               m_id{BBT_COROUTINE_INVALID_PROCESSER_ID};
    volatile ProcesserStatus        m_run_status{ProcesserStatus::PROC_DEFAULT};

    /* 局部队列 */
    CoPriorityQueue                 m_coroutine_queue;

    std::condition_variable         m_run_cond;
    std::atomic_bool                m_run_cond_notify{false}; // 是否需要唤醒
    std::mutex                      m_run_cond_mutex;

    volatile bool                   m_is_running{true};

    Coroutine::Ptr                 m_running_coroutine{nullptr};   // processer当前运行中的协程
    std::atomic_uint64_t            m_running_coroutine_begin{0};      // 当前运行协程开始执行的时间 

    // XXX 可以优化到profiler中
#ifdef BBT_COROUTINE_PROFILE
    uint64_t                        m_co_swap_times{0};
    bbt::core::clock::us                  m_suspend_cost_times{0};
    uint64_t                        m_steal_succ_times{0};  // 偷取数量
    uint64_t                        m_steal_count{0};       // 被偷取数量
#endif
};

}