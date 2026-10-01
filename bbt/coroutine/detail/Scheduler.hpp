#pragma once
#include <atomic>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/utils/lockfree/blockingconcurrentqueue.h>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine::detail
{

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
 */
class Scheduler
{
public:
    friend class Coroutine;
    friend class Profiler;
    friend class Processer;
    typedef std::unique_ptr<Scheduler> UPtr;
    ~Scheduler();

    static UPtr& GetInstance();

    /**
     * @brief 启动运行时（三选一驱动模式）。
     * @throw std::logic_error 已成功启动过或正在启动（并发 Start 同样拒绝）
     */
    void                                        Start(SchedulerStartOpt opt = SCHE_START_OPT_SCHE_THREAD);
    void                                        LoopOnce();

    void                                        RegistCoroutineTask(const CoroutineCallback& handle, const char* desc = nullptr);
    void                                        RegistCoroutineTask(const CoroutineCallback& handle, bool& succ) noexcept;
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
    /* 从全局队列中取一定数量的协程 */
    size_t                                      GetCoroutineFromGlobal(CoroutinePriority priority, CoroutineQueue& queue, size_t size);
    /**
     * @brief 尝试窃取任务（线程安全）
     * 
     * @param thief 窃取者
     * @return 偷取任务数量
     */
    int                                         TryWorkSteal(Processer::SPtr thief);
protected:
    Scheduler();
    void                                        _Init();

    /** Start 状态机：NotStarted → Starting → Initialized；其它转移即重复启动 */
    enum class StartState { NotStarted, Starting, Initialized };

    void                                        _Run();
    /* 定时扫描 */
    void                                        _FixTimingScan();
    void                                        _CreateProcessers();

    bool                                        _LoadBlance2Proc(CoroutinePriority priority, Coroutine::Ptr co);
    /* 初始化全局实例 */
    void                                        _InitGlobalUniqInstance();
    /* worker 就绪后、进入运行循环前发布初始化完成标志 */
    void                                        _PublishInitialized();

    void                                        _OnUpdate();
private:
    /* Scheduler */
    bbt::core::clock::Timestamp<>               m_begin_timestamp;  // 调度器开启时间
    std::thread*                                m_sche_thread{nullptr};
    std::vector<std::thread*>                   m_proc_threads;

    /* Processer 管理 */
    std::map<ProcesserId, Processer::SPtr>      m_processer_map;
    std::vector<Processer::SPtr>                m_load_blance_vec;
    std::atomic<uint32_t>                       m_load_idx{0};
    std::atomic<uint32_t>                       m_steal_idx{0};
    std::mutex                                  m_processer_map_mutex;
    bbt::core::thread::CountDownLatch           m_down_latch;

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

}
