#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <bbt/coroutine/detail/Define.hpp>


namespace bbt::coroutine::detail
{

/* Start 前解析的 worker 数量边界；零值回退只作用于 min/max，不改写原始配置。 */
struct ProcesserScalingBounds
{
    size_t m_initial_num{0};
    size_t m_min_num{0};
    size_t m_max_num{0};
};

/**
 * @brief 协程库静态配置
 * 
 */
class GlobalConfig
{
public:
    typedef std::unique_ptr<GlobalConfig> UPtr;
    static UPtr& GetInstance() {
        /* 进程寿命：配置被 worker/poller 在运行期读取，不在静态退出期析构 */
        static UPtr* _holder = new UPtr(new GlobalConfig());
        return *_holder;
    }

    /* 校验全部扩缩容配置并解析数量边界；失败返回 false，out_bounds 不变。
     * 不启动线程、不修改配置；调用者须在 Start 的不可逆副作用前调用。 */
    bool                                        ResolveProcesserScalingBounds(ProcesserScalingBounds& out_bounds) const noexcept;

    /* 这里的配置都是非线程安全的，建议在启动前设置好 */
    size_t                                      m_cfg_max_coroutine{65535};                 // 最大协程数，超过此数量，继续注册会throw异常

    /**
     * 调度器开始运行时，预创建的静态协程数量。如果不设置，则默认是0，表示不预创建协程。
     * 
     * 如果可以预估协程数量或者想要协程池预热，可以设置此值。
     */
    size_t                                      m_cfg_static_coroutine{0};                  // 如果可以预估协程数量，可以设置此值，默认是0，表示不预估

    size_t                                      m_cfg_stack_size{1024 * 12};                // 栈大小
    bool                                        m_cfg_stack_protect{true};                  // 栈保护
    size_t                                      m_cfg_scan_interval_ms{1};                  // scheduler 扫描间隔
    size_t                                      m_cfg_static_thread_num{0};                 // initial；构造时取 hardware_concurrency()，不可用时取 1
    const size_t                                m_cfg_profile_printf_ms{1000};              // 打印profile间隔，0不打印

    /* Processer */
    /* 启动前配置，运行期模式切换由 Scheduler 控制，不能并发改这些字段。
     * auto 默认关闭，min/max=0 分别回退 initial，保持旧调用方的 worker 数。
     * 数量和 step 在 [1, INT_MAX]；initial 必须在解析后的 [min,max] 内。 */
    bool                                        m_cfg_processer_auto_scale{false};
    size_t                                      m_cfg_processer_min_num{0};
    size_t                                      m_cfg_processer_max_num{0};
    /* interval 毫秒数在 [1, chrono::milliseconds::rep 最大值]；samples >= 1。 */
    size_t                                      m_cfg_processer_scale_interval_ms{100};
    size_t                                      m_cfg_processer_scale_up_samples{3};
    size_t                                      m_cfg_processer_scale_down_samples{3};
    size_t                                      m_cfg_processer_scale_step{1};
    /* busy 为 active worker 中正在 Resume 的百分比，0..100，down <= up。
     * 只作控制信号，不作为 worker 退役安全点，也不依赖 stall 告警开关。 */
    size_t                                      m_cfg_processer_scale_up_busy_percent{80};
    size_t                                      m_cfg_processer_scale_down_busy_percent{20};
    /* 全局 + active 本地 runnable 近似总数：up >= 1，0 <= down <= up。
     * >= up 或 busy >= up 连续命中扩容；<= down 且 busy <= down 连续命中缩容。 */
    size_t                                      m_cfg_processer_scale_up_queue_watermark{1};
    size_t                                      m_cfg_processer_scale_down_queue_watermark{0};

    const size_t                                m_cfg_processer_get_co_from_g_count{16};   // 每次从g队列获取任务数量
    const size_t                                m_cfg_processer_steal_once_min_task_num{8};// 如果有任务，最少偷64个
    const size_t                                m_cfg_processer_worksteal_timeout_ms{10};   // work steal 认为任务饿死的超时时间
    const size_t                                m_cfg_processer_proc_interval_us{3000};     // proc 每次执行间隔时间

    /** CFS 相对服务权重，顺序 LOW/NORMAL/HIGH/CRITICAL；保留旧预算字段名及默认比例。
     *  记账为 elapsed_us / max(1, weight)，零值不再让任务永久失去调度机会。
     *  非线程安全，必须在启动前设置。 */
    std::array<uint64_t, CO_PRIORITY_COUNT>     m_cfg_processer_priority_runtime_budget_us{50, 150, 200, 600};

    /**
     * 一个protected的协程栈的创建需要1次内存申请和一次mprotect系统调用。为了减少开销，使用复用协程栈的方式。
     * 本栈池是支持动态缩容的，下面是控制栈池大小和动态调整的相关参数。
     * 
     * 
     * 下面是相关参数方便在不同场景下进行调整
     */

    /* 栈池中最大可以产生的数量。相当于程序中最多协程数量 */
    const size_t&                               m_cfg_stackpool_max_alloc_size{m_cfg_max_coroutine}; // 栈池中分配最大栈数量

    /* 栈池中达到释放要求的最低协程数量。如果此时程序中总的协程栈数量小于此值，不会进行adjust */
    size_t                                      m_cfg_stackpool_min_alloc_size{128};        // 栈池中最小栈数量
    const size_t                                m_cfg_stackpool_sample_interval{10};        // 采样间隔
    const size_t                                m_cfg_stackpool_adjust_interval{5000};      // 栈池进行动态调整间隔

    /* 异常处理用户hook。非线程安全，bbtco内所有异常都会通过该函数抛出。请保证异常处理函数不会抛出异常 */
    ExceptionHandleCallback                     m_ext_coevent_exception_callback{nullptr};
    /* 未设置异常回调时被吞掉的协程异常计数（默认隔离策略的可观测值） */
    std::atomic_uint64_t                       m_unhandled_exception_count{0};

    /* worker 无进展检测（#277）：>0 启用，协程单次执行超过该毫秒数即告警；
     * 0=关闭诊断采集（默认）；自动扩缩容的轻量 busy 信号独立维护。
     * 检测在调度线程 _FixTimingScan 内，每拍扫描。 */
    size_t                                      m_cfg_worker_stall_warn_ms{0};
    /* 告警回调。未设置时默认 fprintf(stderr) 一行。调度线程调用，勿阻塞。 */
    WorkerStallCallback                         m_ext_worker_stall_callback{nullptr};
private:
    GlobalConfig();
};

} // namespace bbt::coroutine::detail