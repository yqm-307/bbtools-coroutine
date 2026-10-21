#pragma once
#include <condition_variable>
#include <mutex>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/interface/IProcesserDriver.hpp>
#include <bbt/coroutine/utils/lockfree/concurrentqueue.h>

namespace bbt::coroutine::detail
{

/**
 * @brief 一个 Processer 单槽 driver 绑定的「代」（generation）。
 *
 * 每代绑定持有 driver 强引用、可观察状态/故障来源/异常、在途 callback
 * lease 计数（Tick/Wait/Wake/终止 Wake 各占一份）与完成确认 cv。
 * 解绑协议：锁内 state→Detaching 并预留终止 lease → 锁外终止 Wake →
 * 等全部 lease 归零 → 按 binding 身份清槽（槽引用锁外释放）→ 置
 * detached 通知 joiner（detached 表示槽确已解除）；最终 driver 引用
 * 由存活 binding/调用方在所有锁外释放，不在锁内析构。
 */
struct ProcesserDriverBinding
{
    ProcesserId                       m_processer_id{BBT_COROUTINE_INVALID_PROCESSER_ID};
    uint64_t                          m_generation{0};
    std::shared_ptr<IProcesserDriver> m_driver;

    std::mutex                        m_mutex;
    std::condition_variable           m_cv;
    ProcesserDriverState              m_state{ProcesserDriverState::Bound};
    ProcesserDriverFailure            m_failure{ProcesserDriverFailure::None};
    std::exception_ptr                m_exception{};
    size_t                            m_inflight{0};
    bool                              m_terminal_wake_reserved{false}; // 终止 Wake 只预留一次
    bool                              m_detached{false};               // 该代归属已解除
};

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
    /* 只读取同一份线程局部存储；不存在时返回空，不创建 Processer。 */
    static SPtr TryGetLocalProcesser();

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
    size_t                          GetLoadValue();
    size_t                          GetExecutableNum(); /* 可执行协程数 */
    /* admission gate：返回 false 表示 P 已关闭接纳（Preparing/Retiring/
     * Exited），调用方仍持有协程所有权，必须重试 active/global 或显式拒绝。
     * 入队与关闭 admission 通过 in-flight 计数线性化，旧 snapshot 命中
     * 不能仅凭 shared_ptr 保活就写 retired 本地队列。 */
    bool                            AddCoroutineTask(CoroutinePriority priority, Coroutine::Ptr coroutine);

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
    /* enqueue 之后的锁内通知：只置 latch/cond，不调外部 driver */
    void                            _PublishRunnable();
    /* 组合入口：发布 + 锁外 driver Wake（deferral 生效时只记 pending） */
    void                            _NotifyRunnable();
    /* 锁外调用 driver->Wake()；Defer 命中时仅登记 pending */
    void                            _WakeDriver();
    void                            _WaitForWork();
    /* driver 槽内部入口（仅 Scheduler 经 friend 调用） */
    ProcesserDriverStatus           _BindDriver(const std::shared_ptr<IProcesserDriver>& driver,
                                                ProcesserDriverToken& out_token);
    ProcesserDriverStatus           _QueryDriver(ProcesserDriverSnapshot& out_snapshot);
    ProcesserDriverStatus           _UnregisterDriver(uint64_t generation);

    /* ---- 动态 worker 生命周期（仅 Scheduler 经 friend 调用）----
     * Preparing -> Active -> Retiring -> Exited -> Joined：
     *  - Preparing：本批线程尚未 commit，不接任务/不绑定 driver；
     *  - Active：active 快照成员，admission 开放；
     *  - Retiring：已从 active 移除、admission 关闭；worker 完成当前
     *    Resume 的 READY/EVENT_WAIT/FINAL disposition 后排空本地队列；
     *  - Exited：调度函数已发布退出，不等于线程返回/TLS 析构/join；
     *  - Joined：reaper 在锁外 join 返回后才发布。
     * Scheduler 提交 Active/Retiring，worker 发布 Exited，reaper 发布 Joined；
     * admission 关闭与 enqueue/drain 握手使用 SC 全序，其余状态 release 发布。 */
    static void                     _InstallLocalProcesser(SPtr proc);
    void                            _Activate();
    void                            _BeginRetire();
    bool                            _IsActive() const noexcept;
    /* Bound/Failed/Detaching 任一有归属状态都占 slot，均 pin 到同代清槽 */
    bool                            _HasDriverOwnership();
    /* worker 自身调度栈：等 admission in-flight 归零后实际 dequeue 各优先级
     * 本地 runnable，逐项可靠转交全局；失败不丢所有权。 */
    void                            _DrainLocalToGlobal();
    void                            _MarkJoined();
private:
    /* 带 notify 开关的 admission 入队：AddCoroutineTask 用于外部生产者
     * （需要 _PublishRunnable 唤醒）；worker 自身在持 run_cond 的睡前复查
     * 或 steal 目的端使用 notify=false，避免自锁。 */
    bool                            _EnqueueAdmitted(CoroutinePriority priority, Coroutine::Ptr coroutine, bool notify);
    void                            _ReleaseEnqueueSlot();
    void                            _WaitEnqueueQuiescent();
    std::shared_ptr<ProcesserDriverBinding> _GetDriverBinding();
    void                            _DriverOnTick(ProcesserDriverPhase phase);
    void                            _DriverFail(const std::shared_ptr<ProcesserDriverBinding>& binding,
                                                ProcesserDriverFailure source,
                                                std::exception_ptr err);

    static SPtr&                    _GetLocalProcesserStorage();
    /* 控制信息 */
    const ProcesserId               m_id{BBT_COROUTINE_INVALID_PROCESSER_ID};
    std::atomic<ProcesserStatus>    m_run_status{ProcesserStatus::PROC_DEFAULT};

    /* 生命周期/admission：与 m_run_status 正交。m_admission_open 是本地
     * 队列的唯一接纳门；m_retire_requested 由 worker 在自身调度栈确认。 */
    std::atomic<ProcesserLifecycleState> m_lifecycle{ProcesserLifecycleState::Preparing};
    std::atomic_bool                m_admission_open{false};
    std::atomic_bool                m_retire_requested{false};
    /* 轻量 busy 信号：每次 Resume 前后维护，独立于 stall_warn_ms。 */
    std::atomic_bool                m_busy{false};
    /* admission 关闭时等待在途 enqueue 收敛（生产者每次入队 ++/--）。 */
    std::atomic_int                 m_enqueue_inflight{0};
    std::mutex                      m_enqueue_wait_mutex;
    std::condition_variable         m_enqueue_wait_cv;

    /* 调度相关 */
    CoPriorityQueue                 m_coroutine_queue;
    std::condition_variable         m_run_cond;
    std::atomic_bool                m_run_cond_notify{false}; // 是否需要唤醒
    std::mutex                      m_run_cond_mutex;

    /* 运行时相关 */
    std::atomic_bool                m_is_running{true};
    Coroutine::Ptr                  m_running_coroutine{nullptr};   // processer当前运行中的协程
    std::atomic_uint64_t            m_running_coroutine_begin{0};   // 当前运行协程开始执行的时间

    /* ---- worker 无进展快照（#277/#367）----
     * 单写者=worker 线程（协程每次进入 Resume 前），读者=调度线程。
     * 同步依据：全部字段受 m_exec_mutex 保护，写者在临界区内一次性提交，
     * 读者在临界区内整组复制——快照一致性直接由互斥锁的 happens-before
     * 给出，每次上报载荷同属一次 Resume，无需无锁协议的正确性论证。
     * 诊断可选且非热路径，取可证明的最小同步。
     * m_last_stall_report_begin 仅调度线程读写。 */
    std::mutex                      m_exec_mutex;                   // 以下快照字段均由它保护
    bool                            m_executing{false};             // 正在 Resume 用户协程中
    uint64_t                        m_exec_co_id{0};
    uint64_t                        m_exec_begin_us{0};
    uint64_t                        m_exec_backlog{0};
    char                            m_exec_desc[56]{};
    uint64_t                        m_last_stall_report_begin{0};   // 调度线程专用（去重）
    static constexpr int            kGlobalCheckInterval = 4;       // 每N轮无条件检查全局队列
    int                             m_global_check_counter{0};

    /* driver 单槽：m_driver_slot_mutex 只保护槽位本身；binding 内部状态
     * 由 binding->m_mutex 保护。锁序：slot → binding，均不反向依赖
     * run_cond/global/map 锁。 */
    std::mutex                                  m_driver_slot_mutex;
    std::shared_ptr<ProcesserDriverBinding>     m_driver_binding;
    uint64_t                                    m_driver_next_generation{0};

    // XXX 可以优化到profiler中
#ifdef BBT_COROUTINE_PROFILE
    /* ProfileInfo 可在 worker 运行/退役时并发采样，计数不与其它状态组成快照。 */
    std::atomic_uint64_t            m_co_swap_times{0};
    std::atomic_uint64_t            m_suspend_cost_times{0}; // 微秒
    std::atomic_uint64_t            m_steal_succ_times{0};   // 偷取数量
    std::atomic_uint64_t            m_steal_count{0};        // 被偷取数量
    std::atomic_uint64_t            m_stall_loop_count{0};   // 本地队列空但size_approx非零的次数
#endif
};

}