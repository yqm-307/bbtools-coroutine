/**
 * Processer：worker 线程的执行单元。本地 CFS 就绪队列、admission 门禁、
 * 生命周期状态和单槽 driver 绑定都在这里；创建/激活/退役由 Scheduler 驱动。
 */
#pragma once
#include <condition_variable>
#include <mutex>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/interface/IProcesserDriver.hpp>
#include <bbt/coroutine/utils/lockfree/concurrentqueue.h>

namespace bbt::coroutine::detail {

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
struct ProcesserDriverBinding {
    ProcesserId m_processer_id{BBT_COROUTINE_INVALID_PROCESSER_ID};
    uint64_t m_generation{0};
    std::shared_ptr<IProcesserDriver> m_driver;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    ProcesserDriverState m_state{ProcesserDriverState::Bound};
    ProcesserDriverFailure m_failure{ProcesserDriverFailure::None};
    std::exception_ptr m_exception{};
    size_t m_inflight{0};

    /**
     * 终止 Wake 只预留一次
     */
    bool m_terminal_wake_reserved{false};

    /**
     * 该代归属已解除
     */
    bool m_detached{false};
};

/**
 * @brief 一个 worker 线程的执行单元：本地就绪队列 + admission 门禁 + 单槽 driver。
 *
 * 一个 P 绑定一个线程；TLS 那份是本线程唯一可见的 P，Scheduler 的 worker 由
 * _WorkerMain 指向已登记对象，不会在 worker 里再建第二个。
 *
 * 控制权不重叠：本地队列只由本线程 Pick/Account、外部生产者过 admission 入队、
 * steal/排空走 Take；生命周期与 admission 只由 Scheduler 在 m_lifecycle_mutex 下
 * 推进（worker 只发布 Exited，reaper 在 join 后发布 Joined）；driver 槽绑定在
 * lifecycle 锁下提交、解绑走同代校验。无停机入口，_Run 只在 retire 后退出。
 */
class Processer : public std::enable_shared_from_this<Processer> {
public:
    friend class Scheduler;
    friend class Profiler;
    typedef std::shared_ptr<Processer> SPtr;

    /**
     * 只构造对象：分配 id、置 PROC_SUSPEND，不装 TLS、不进 active 快照；参与调度须由 Scheduler prepare→commit。
     */
    static SPtr Create();

    /**
     * 取当前线程的 P；不存在时惰性建一个只在本线程 TLS 可见、未登记 Scheduler 的 P：
     * 不进 active 快照，不能当 driver 目标或定向首投目标（未初始化时也会创建）。
     */
    static SPtr GetLocalProcesser();

    /**
     * 只读取同一份线程局部存储；不存在时返回空，不创建 Processer。
     */
    static SPtr TryGetLocalProcesser();

    explicit Processer();
    ~Processer();

    /**
     * 最近一次调度状态：worker 每轮调度时写，任意线程可读（atomic）。
     */
    ProcesserStatus GetStatus();

    /**
     * 构造时分配的 id，进程内不复用；只读，任意线程可读。
     */
    ProcesserId GetId();

    /**
     * 只在 P 自己的 worker 线程上有意义：m_running_coroutine 非原子，跨线程读是竞争；不在 Resume 中返回 nullptr。
     */
    Coroutine::Ptr GetCurrentCoroutine();

protected:
    /**
     * 非公开库内部接口
     */

    /**
     * 跑本对象的调度循环：background_thread=true 新建并 detach 一条线程（不可 join），false 在当前线程同步跑；
     * Scheduler 的 worker 不走这里，由 _WorkerMain 直接调 _Run。
     */
    void Start(bool background_thread = true);
    size_t GetLoadValue();

    /**
     * 可执行协程数
     */
    size_t GetExecutableNum();

    /**
     * admission gate：false 表示 P 已关闭接纳（Preparing/Retiring/Exited），协程所有权仍在调用方，
     * 须重试 active/global 或显式拒绝；入队与关闭通过 in-flight 计数线性化。
     */
    bool AddCoroutineTask(CoroutinePriority priority, Coroutine::Ptr coroutine);

    /**
     * 协程上下文换出次数
     */
    uint64_t GetContextSwapTimes();

    /**
     * 任务执行耗时，返回微秒
     */
    uint64_t GetSuspendCostTime();

    /**
     * 窃取他人成功次数
     */
    uint64_t GetStealSuccTimes();

    /**
     * 被窃取次数
     */
    uint64_t GetStealCount();

    /**
     * 空队列但size_approx非零的循环数
     */
    uint64_t GetStallLoopCount();

    /**
     * @brief 从 *this（victim）转移一批本地 runnable 给 thief；victim 须 Active 且 admission
     * 开、thief 须 Active，否则返回 0。thief 拒收的那一项回全局队列，不丢所有权。
     * @param proc 窃取方
     */
    size_t Steal(Processer::SPtr proc);

protected:
    void _Init();
    static ProcesserId _GenProcesserId();
    size_t _TryGetCoroutineFromGlobal();
    void _Run();

    /**
     * enqueue 之后的锁内通知：只置 latch/cond，不调外部 driver
     */
    void _PublishRunnable();

    /**
     * 组合入口：发布 + 锁外 driver Wake（deferral 生效时只记 pending）
     */
    void _NotifyRunnable();

    /**
     * 锁外调用 driver->Wake()；Defer 命中时仅登记 pending
     */
    void _WakeDriver();
    void _WaitForWork();

    /**
     * driver 槽内部入口（仅 Scheduler 经 friend 调用）
     */
    ProcesserDriverStatus _BindDriver(const std::shared_ptr<IProcesserDriver>& driver, ProcesserDriverToken& out_token);
    ProcesserDriverStatus _QueryDriver(ProcesserDriverSnapshot& out_snapshot);
    ProcesserDriverStatus _UnregisterDriver(uint64_t generation);

    /**
     * ---- 动态 worker 生命周期（仅 Scheduler 经 friend 调用）----
     * Preparing → Active → Retiring → Exited → Joined。Scheduler 提交 Active/Retiring，
     * worker 发布 Exited（只表示调度函数已退出，不等于线程返回或 join），reaper 在 join
     * 返回后发布 Joined。Retiring 关闭 admission 并排空本地队列。
     */
    static void _InstallLocalProcesser(SPtr proc);
    void _Activate();
    void _BeginRetire();
    bool _IsActive() const noexcept;

    /**
     * Bound/Failed/Detaching 任一有归属状态都占 slot，均 pin 到同代清槽
     */
    bool _HasDriverOwnership();

    /**
     * worker 自身调度栈：等 admission in-flight 归零后实际 dequeue 各优先级
     * 本地 runnable，逐项可靠转交全局；失败不丢所有权。
     */
    void _DrainLocalToGlobal();
    void _MarkJoined();

private:
    /**
     * 带 notify 开关的 admission 入队：外部生产者要 notify=true 唤醒 worker；worker 自身在
     * 睡前复查或 steal 目的端用 false，避免自锁。
     */
    bool _EnqueueAdmitted(CoroutinePriority priority, Coroutine::Ptr coroutine, bool notify);
    void _ReleaseEnqueueSlot();
    void _WaitEnqueueQuiescent();
    std::shared_ptr<ProcesserDriverBinding> _GetDriverBinding();
    void _DriverOnTick(ProcesserDriverPhase phase);
    void _DriverFail(const std::shared_ptr<ProcesserDriverBinding>& binding, ProcesserDriverFailure source,
                     std::exception_ptr err);

    static SPtr& _GetLocalProcesserStorage();

    /**
     * 控制信息
     */
    const ProcesserId m_id{BBT_COROUTINE_INVALID_PROCESSER_ID};
    std::atomic<ProcesserStatus> m_run_status{ProcesserStatus::PROC_DEFAULT};

    /**
     * 生命周期/admission：与 m_run_status 正交。m_admission_open 是本地
     * 队列的唯一接纳门；m_retire_requested 由 worker 在自身调度栈确认。
     */
    std::atomic<ProcesserLifecycleState> m_lifecycle{ProcesserLifecycleState::Preparing};
    std::atomic_bool m_admission_open{false};
    std::atomic_bool m_retire_requested{false};

    /**
     * 轻量 busy 信号：每次 Resume 前后维护，独立于 stall_warn_ms。
     */
    std::atomic_bool m_busy{false};

    /**
     * admission 关闭时等待在途 enqueue 收敛（生产者每次入队 ++/--）。
     */
    std::atomic_int m_enqueue_inflight{0};
    std::mutex m_enqueue_wait_mutex;
    std::condition_variable m_enqueue_wait_cv;

    /**
     * 调度相关
     */
    CfsRunQueue m_coroutine_queue;
    std::condition_variable m_run_cond;

    /**
     * 是否需要唤醒
     */
    std::atomic_bool m_run_cond_notify{false};
    std::mutex m_run_cond_mutex;

    /**
     * 运行时相关
     */
    std::atomic_bool m_is_running{true};

    /**
     * processer当前运行中的协程
     */
    Coroutine::Ptr m_running_coroutine{nullptr};

    /**
     * 当前运行协程开始执行的时间
     */
    std::atomic_uint64_t m_running_coroutine_begin{0};

    /**
     * ---- worker 无进展快照 ----
     * 单写者=worker（Resume 前），读者=调度线程；字段受 m_exec_mutex 保护，写者一次提交、
     * 读者整组复制，一致性由锁的 happens-before 给出。m_last_stall_report_begin 仅调度线程读写。
     *
     * 以下快照字段均由它保护
     */
    std::mutex m_exec_mutex;

    /**
     * 正在 Resume 用户协程中
     */
    bool m_executing{false};
    uint64_t m_exec_co_id{0};
    uint64_t m_exec_begin_us{0};
    uint64_t m_exec_backlog{0};
    char m_exec_desc[56]{};

    /**
     * 调度线程专用（去重）
     */
    uint64_t m_last_stall_report_begin{0};

    /**
     * 每N轮无条件检查全局队列
     */
    static constexpr int kGlobalCheckInterval = 4;
    int m_global_check_counter{0};

    /**
     * worker 独占的轮转游标
     */
    int m_global_pull_priority{CO_PRIORITY_CRITICAL};

    /**
     * driver 单槽：m_driver_slot_mutex 只保护槽位本身；binding 内部状态
     * 由 binding->m_mutex 保护。锁序：slot → binding，均不反向依赖
     * run_cond/global/map 锁。
     */
    std::mutex m_driver_slot_mutex;
    std::shared_ptr<ProcesserDriverBinding> m_driver_binding;
    uint64_t m_driver_next_generation{0};

    /**
     * 计数随 P 生灭，由 Profiler 汇总读取；按 P 独立开关采集的话再搬进它的 worker registry。
     */
#ifdef BBT_COROUTINE_PROFILE

    /**
     * ProfileInfo 可在 worker 运行/退役时并发采样，计数不与其它状态组成快照。
     */
    std::atomic_uint64_t m_co_swap_times{0};

    /**
     * 微秒
     */
    std::atomic_uint64_t m_suspend_cost_times{0};

    /**
     * 偷取数量
     */
    std::atomic_uint64_t m_steal_succ_times{0};

    /**
     * 被偷取数量
     */
    std::atomic_uint64_t m_steal_count{0};

    /**
     * 本地队列空但size_approx非零的次数
     */
    std::atomic_uint64_t m_stall_loop_count{0};
#endif
};

}