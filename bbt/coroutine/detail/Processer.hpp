#pragma once
#include <condition_variable>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/utils/lockfree/concurrentqueue.h>

namespace bbt::coroutine::detail
{

/**
 * @brief Processer类
 * 
 * Processer是协程的执行单元，负责调度和执行协程任务。
 * 每个Processer绑定一个线程，且每个线程只能有一个Processer。
 * 
 * Processer的主要职责包括：
 * - 执行协程任务
 * - 管理协程的状态
 * - 支持协程的窃取和负载均衡
 * 
 */
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
    uint64_t                        GetStallLoopCount(); /* 空队列但size_approx非零的循环数 */
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
    /* 控制信息 */
    const ProcesserId               m_id{BBT_COROUTINE_INVALID_PROCESSER_ID};
    std::atomic<ProcesserStatus>    m_run_status{ProcesserStatus::PROC_DEFAULT};

    /* 调度相关 */
    CoPriorityQueue                 m_coroutine_queue;
    std::condition_variable         m_run_cond;
    std::atomic_bool                m_run_cond_notify{false}; // 是否需要唤醒
    std::mutex                      m_run_cond_mutex;

    /* 运行时相关 */
    std::atomic_bool                m_is_running{true};
    std::atomic_bool                m_is_shutdown{false};  // 强制关闭标志，跳过协程执行
    Coroutine::Ptr                  m_running_coroutine{nullptr};   // processer当前运行中的协程
    std::atomic_uint64_t            m_running_coroutine_begin{0};   // 当前运行协程开始执行的时间

    /* ---- worker 无进展快照（#277）----
     * 单写者=worker 线程（协程每次进入 Resume 前），读者=调度线程。
     * seqlock 协议：写前 seq->1（奇），写完 seq->2（偶，此后字段稳定直到
     * 下一轮）。读者只在 executing==true 且 seq 为偶、前后两读一致时使用
     * 字段；m_last_stall_report_begin 仅调度线程读写。全部 plain 字段配合
     * seq/atomic fence，无需更重的同步——诊断数据，允许跳过一拍。 */
    std::atomic_bool                m_executing{false};             // 正在 Resume 用户协程中
    std::atomic_uint64_t            m_exec_seq{0};                  // seqlock 序号
    uint64_t                        m_exec_co_id{0};                // 以下字段被 seqlock 保护
    uint64_t                        m_exec_begin_us{0};
    uint64_t                        m_exec_backlog{0};
    char                            m_exec_desc[56]{};
    uint64_t                        m_last_stall_report_begin{0};   // 调度线程专用（去重）
    static constexpr int            kGlobalCheckInterval = 4;       // 每N轮无条件检查全局队列
    int                             m_global_check_counter{0};

    // XXX 可以优化到profiler中
#ifdef BBT_COROUTINE_PROFILE
    uint64_t                        m_co_swap_times{0};
    bbt::core::clock::us            m_suspend_cost_times{0};
    uint64_t                        m_steal_succ_times{0};  // 偷取数量
    uint64_t                        m_steal_count{0};       // 被偷取数量
    uint64_t                        m_stall_loop_count{0};  // 本地队列空但size_approx非零的次数
#endif
};

}