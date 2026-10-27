#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/utils/lockfree/blockingconcurrentqueue.h>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine::detail
{

struct ProcesserScalingBounds;

/**
 * @brief 调度器
 *
 * Scheduler是协程的调度中心，负责管理和调度所有协程任务。
 *
 * Scheduler的主要职责包括：
 *  - 管理Processer（协程执行单元）
 *  - 管理全局协程队列
 *  - 负载均衡和任务窃取
 *  - 提供协程注册和激活接口
 *  - 事件派发
 *  - 栈池动态调整
 *
 *
 * Scheduler本身压力较小，希望如果后续有调整放在Scheduler中
 *
 * 执行模型：多 Processer 并行，每线程一个；单个协程同一时刻只在
 * 一个 Processer 上 Resume；挂起后可入全局队列并被其他 Processer 取出。
 * TLS（g_bbt_tls_processer / g_bbt_tls_coroutine_co）只在 Processer 线程、
 * 当前协程栈上有效。
 * 稳定入口：GetInstance / Start / LoopOnce / RegistCoroutineTask / IsInitialized。
 * g_scheduler 即 GetInstance()，本阶段单例，进程寿命（不在静态退出期析构）。
 * Start 一次：成功初始化后重复 Start 抛 std::logic_error，不再重置配置/队列或新增 worker。
 * 没有业务停机入口：不提供 Stop/restart，挂起协程不做栈展开的语义不变。
 * Start(THREAD) 后不要 LoopOnce。
 * EventLoop 只通过 CoPoller::PollOnce 驱动。不直接依赖 Poller/epoll/ASIO。
 * 换 backend 改 CoPoller，不改本头。
 *
 * ---- 动态扩缩容的字段/所有权/锁序总表（实现说明，契约 §6）----
 * 字段                                所有者/写者                 读者                      发布点
 * m_active_snapshot (shared_ptr<const vector>)  扩缩容提交（m_processer_map_mutex 下）  投递/steal/广播/诊断  提交时整体 swap
 * m_processer_map                    扩缩容提交（map 锁下）      定向查找/绑定/查询       同一次提交
 * m_workers (WorkerRecord)           扩缩容 prepare/commit、reaper   reaper/诊断             提交 append；join 后 erase
 * Processer::m_lifecycle/admission   Scheduler（m_lifecycle_mutex 下） 所有投递路径           atomic release
 * Processer 本地队列                 worker 自身 / admission gate 保活      worker/steal          enqueue 后 _PublishRunnable
 *
 * 有向锁序（只能从左到右）：
 *   m_start_mutex
 *     -> m_lifecycle_mutex（resize 串行 + driver 绑定/退役互斥）
 *       -> m_processer_map_mutex（registry/snapshot/workers）
 *         -> m_global_queue_mutex（仅普通投递；drain 不持 map）
 *         -> m_run_cond_mutex（唤醒 worker）
 *         -> Processer::m_driver_slot_mutex -> binding->m_mutex
 *   反向只允许“释放后再取”，例如 worker 睡前复查在 run_cond 内只做无锁
 *   global try_dequeue，不再取 map/lifecycle；reaper 在 join 前释放全部锁。
 * 旧 snapshot/map 命中只保对象寿命；真正写本地队列前必须重新通过
 * admission/state gate（AddCoroutineTask / _TryGetCoroutineFromGlobal /
 * Steal / 睡前复查 / driver Wake 路径均遵守）。
 */
class Scheduler
{
private:
    /* 扩容批次启动门：全部 worker 线程创建成功并完成 registry 提交后
     * 才一次放行；失败只取消/join 未公开批次。 */
    struct BatchStartGate
    {
        std::mutex                  m_mutex;
        std::condition_variable     m_cv;
        bool                        m_started{false};
        bool                        m_cancel{false};
        size_t                      m_ready_num{0};
        bool                        m_failed{false};
    };

    /* 一个物理 worker 的稳定记录：线程 join 前由 m_workers 持有；
     * Exited 由 worker 发布，Joined 只在锁外 join 返回后发布。 */
    struct WorkerRecord
    {
        Processer::SPtr             m_processer;
        std::thread*                m_thread{nullptr};
        std::atomic_bool            m_claimed{false};  // reaper 已认领，避免重复 join
        std::promise<void>          m_exit_promise;
        std::future<void>           m_thread_exited{m_exit_promise.get_future()};
    };

    typedef std::shared_ptr<WorkerRecord> WorkerRecordSPtr;
    typedef std::vector<Processer::SPtr> ProcesserList;

    static constexpr size_t kMaxOutstandingRetire = 32;  // 未完成退役/未 join 批次上限
    static constexpr size_t kMaxReapPerTick = 8;         // 每次有限推进的 join 数量

public:
    friend class Coroutine;
    friend class Profiler;
    friend class Processer;
    friend class DriverWakeDeferral;
    typedef std::unique_ptr<Scheduler> UPtr;
    ~Scheduler();

    static UPtr& GetInstance();

    /**
     * @brief 启动运行时（三选一驱动模式）。
     * @throw std::invalid_argument 扩缩容配置非法（无副作用，修正后可重试）
     * @throw std::logic_error 已成功启动过或正在启动（并发 Start 同样拒绝）
     * @throw std::runtime_error worker 批次创建失败（未公开任何半批 worker）
     */
    void                                        Start(SchedulerStartOpt opt = SCHE_START_OPT_SCHE_THREAD);
    void                                        LoopOnce();

    void                                        RegistCoroutineTask(const CoroutineCallback& handle, const char* desc = nullptr);
    void                                        RegistCoroutineTask(const CoroutineCallback& handle, bool& succ) noexcept;
    void                                        RegistCoroutineTask(size_t stack_size, const CoroutineCallback& handle, const char* desc = nullptr);
    void                                        RegistCoroutineTask(size_t stack_size, const CoroutineCallback& handle, bool& succ) noexcept;

    /**
     * @brief 定向首投：只把协程放入目标已登记 worker 的初始队列，
     *  不承诺首次 Resume 在目标执行（stealing 与挂起恢复路径不变）。
     * @throw std::logic_error 未初始化；std::invalid_argument 非法或已退役目标；
     *  分配失败沿用 RegistCoroutineTask 既有异常
     */
    void                                        RegistCoroutineTaskOn(ProcesserId target, const CoroutineCallback& handle, const char* desc = nullptr);
    void                                        RegistCoroutineTaskOn(ProcesserId target, size_t stack_size, const CoroutineCallback& handle, const char* desc = nullptr);

    /* ---- processer driver adapter（契约 §11，见 IProcesserDriver.hpp）----
     * 注册失败不改 out_token；查询失败不改 out_snapshot。 */
    std::vector<ProcesserId>                    GetWorkerProcesserIds();
    ProcesserDriverStatus                       RegisterProcesserDriver(ProcesserId target,
                                                    std::shared_ptr<IProcesserDriver> driver,
                                                    ProcesserDriverToken& out_token);
    ProcesserDriverStatus                       QueryProcesserDriver(ProcesserId target,
                                                    ProcesserDriverSnapshot& out_snapshot);
    ProcesserDriverStatus                       UnregisterProcesserDriver(const ProcesserDriverToken& token);

    /* ---- Processer 动态扩缩容控制/观察（契约 §6.1–§6.3）----
     * 返回值只表示“目标是否已提交”；Ok/Pending 都不是物理线程已 join 的收据。
     * 合法手动请求（含 same-target）切 Manual 并复位双向样本；未初始化、
     * 越界、driver 约束、创建失败、SelfRetire、Busy 不改变 target/mode/样本。
     * 手动与自动、扩/缩共用同一串行入口。 */
    ProcesserResizeResult                       SetProcesserNum(size_t num);
    ProcesserResizeResult                       SetProcesserAutoScale(bool enable);
    /* active 是权威活动集合（已扣除 Retiring/Exited），不是物理线程总数。 */
    size_t                                      GetProcesserNum();
    size_t                                      GetProcesserTargetNum();
    size_t                                      GetProcesserRetiringNum();
    size_t                                      GetProcesserExitedUnjoinedNum();
    ProcesserScalingMode                        GetProcesserScalingMode();

    /* 协程被激活，重新加入全局队列 */
    void                                        OnActiveCoroutine(CoroutinePriority priority, Coroutine::Ptr coroutine);

    /**
     * @brief 运行时是否已完成必要初始化（runtime/worker/poller）。
     *
     * 初始化完成后恒为 true，不因业务关闭归零——进程寿命内不会再有
     * “已停止”状态。初始化标志在进入运行循环前发布。
     */
    bool                                        IsInitialized() const noexcept
    {
        return m_initialized.load(std::memory_order_acquire);
    }

protected:
    /* 从全局队列中取一定数量的协程；逐项通过目标 worker 的 admission
     * gate（关闭则把该项送回全局并停止），避免 global pull 写入 retired P。 */
    size_t                                      GetCoroutineFromGlobal(CoroutinePriority priority, size_t size, Processer& owner);
    /**
     * @brief 尝试窃取任务（线程安全）
     *
     * @param thief 窃取者
     * @return 偷取任务数量
     */
    int                                         TryWorkSteal(Processer::SPtr thief);
protected:
    Scheduler();
    void                                        _Init(const ProcesserScalingBounds& bounds);

    /** Start 状态机：NotStarted → Starting → Initialized；其它转移即重复启动 */
    enum class StartState { NotStarted, Starting, Initialized };

    void                                        _Run();
    /* 定时扫描 */
    void                                        _FixTimingScan();
    /* 创建 initial 个 worker；失败时本批已创建线程全部取消/join，不公开半批 */
    bool                                        _CreateProcessers(size_t initial_num);
    /* 新线程入口：等批次放行后安装 TLS Processer 并运行 */
    void                                        _WorkerMain(WorkerRecordSPtr record, std::shared_ptr<BatchStartGate> gate);
    /* 取消未公开批次：置 cancel、唤醒、join、释放线程对象 */
    void                                        _CancelBatch(std::shared_ptr<BatchStartGate>& gate,
                                                    std::vector<WorkerRecordSPtr>& records);

    /* 以下 *Locked 入口由调用方持有 m_lifecycle_mutex。
     * 返回的 out_notify 必须在退出全部锁后逐个 _PublishRunnable。 */
    ProcesserResizeResult                       _ApplyTargetLocked(size_t target, std::vector<Processer::SPtr>& out_notify,
                                                    std::unique_lock<std::mutex>& lifecycle_lock);
    bool                                        _PrepareExpansionLocked(size_t need, std::vector<Processer::SPtr>& out_new,
                                                    std::unique_lock<std::mutex>& lifecycle_lock);
    ProcesserResizeResult                       _CommitShrinkLocked(size_t need, size_t target,
                                                    std::vector<Processer::SPtr>& out_notify);
    size_t                                      _ActiveCountLocked();
    size_t                                      _CountOutstandingRetireLocked();  // 调用方另持 map 锁
    void                                        _ResetScalingSamplesLocked();

    /* 每采样拍驱动自动策略；NO_LOOP 由 LoopOnce 调用 */
    void                                        _AutoScaleTick();
    /* 有限推进 reaper：只认领 Exited，join 全程不持 runtime/driver/队列锁 */
    void                                        _ReapRetiredWorkers();
    /* 唤醒当前 active worker（drain 后把全局任务交给剩余 worker） */
    void                                        _WakeActiveWorkers();
    /* worker drain 的全局入队（可靠重试由调用方负责） */
    bool                                        _EnqueueGlobal(CoroutinePriority priority, Coroutine::Ptr coroutine);

    /* 锁内完成挑选+入队+发布；返回已移交的稳定 worker（失败为 nullptr），
     * 由调用方在退出全部锁后 _WakeDriver */
    Processer::SPtr                             _LoadBlance2Proc(CoroutinePriority priority, Coroutine::Ptr co);
    /* 延期广播入口：仅 DriverWakeDeferral 最外层析构调用，遍历 active
     * 快照补发 driver Wake；不重新发布任务/latch */
    static void                                 _WakeProcesserDrivers();
    /* 初始化全局实例 */
    void                                        _InitGlobalUniqInstance();
    /* worker 就绪后、进入运行循环前发布初始化完成标志 */
    void                                        _PublishInitialized();

    void                                        _OnUpdate();
private:
    /* Scheduler */
    bbt::core::clock::Timestamp<>               m_begin_timestamp;  // 调度器开启时间
    std::thread*                                m_sche_thread{nullptr};

    /* Processer 管理（active 权威集合 + 物理 worker 记录） */
    std::map<ProcesserId, Processer::SPtr>      m_processer_map;
    std::shared_ptr<const ProcesserList>        m_active_snapshot;
    std::vector<WorkerRecordSPtr>               m_workers;
    std::atomic<uint32_t>                       m_load_idx{0};
    std::atomic<uint32_t>                       m_steal_idx{0};
    std::mutex                                  m_processer_map_mutex;
    /* resize 串行 + driver 绑定/退役互斥；不跨 join/外部回调持有 */
    std::mutex                                  m_lifecycle_mutex;

    /* 扩缩容策略状态（m_lifecycle_mutex 下写；Start 前初始化） */
    size_t                                      m_min_num{1};
    size_t                                      m_max_num{1};
    std::atomic<size_t>                         m_target_num{0};
    std::atomic<ProcesserScalingMode>           m_scaling_mode{ProcesserScalingMode::Manual};
    size_t                                      m_scale_interval_ms{100};
    size_t                                      m_scale_up_samples{3};
    size_t                                      m_scale_down_samples{3};
    size_t                                      m_scale_step{1};
    size_t                                      m_scale_up_busy_percent{80};
    size_t                                      m_scale_down_busy_percent{20};
    size_t                                      m_scale_up_queue_watermark{1};
    size_t                                      m_scale_down_queue_watermark{0};
    size_t                                      m_scale_up_hits{0};
    size_t                                      m_scale_down_hits{0};
    std::chrono::steady_clock::time_point       m_last_auto_sample{};

    /* coroutine全局队列 */
    CoPriorityQueue                             m_global_coroutine_queue;
    /* 外部 Notify 的在途入队必须串行，避免与队列遍历并发。 */
    std::mutex                                  m_global_queue_mutex;
    /* Start 一次性：状态转移与检查同锁，布尔字段不足以拒绝并发 Start。 */
    std::mutex                                  m_start_mutex;
    StartState                                  m_start_state{StartState::NotStarted};
    std::atomic_bool                            m_initialized{false};

    uint64_t                                    m_regist_coroutine_count{0};
};

} // namespace bbt::coroutine::detail
