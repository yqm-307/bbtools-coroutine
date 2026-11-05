/* 协程库静态配置：字段由使用者在 Start 前写好，运行期只读。 */
#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail {

/* Start 前解析的 worker 数量边界；零值回退只作用于 min/max，不改写原始配置。 */
struct ProcesserScalingBounds {
    size_t m_initial_num{0};
    size_t m_min_num{0};
    size_t m_max_num{0};
};

/** @brief 协程库静态配置
 * 字段在 Start 之前设置，之后被 worker、poller 与调度线程直接读取、不加锁；唯一的并发字段是
 * m_unhandled_exception_count。实例是进程寿命对象，不在静态退出期析构。 */
class GlobalConfig {
public:
    typedef std::unique_ptr<GlobalConfig> UPtr;
    static UPtr& GetInstance()
    {
        /* 进程寿命：配置被 worker/poller 在运行期读取，不在静态退出期析构 */
        static UPtr* _holder = new UPtr(new GlobalConfig());
        return *_holder;
    }

    /* 校验全部扩缩容配置并解析数量边界；失败返回 false，out_bounds 不变。
     * 不启动线程、不修改配置；调用者须在 Start 的不可逆副作用前调用。 */
    bool ResolveProcesserScalingBounds(ProcesserScalingBounds& out_bounds) const noexcept;

    /* 这里的配置都是非线程安全的，建议在启动前设置好 */
    /* 同时存活的协程栈上限，经 m_cfg_stackpool_max_alloc_size 生效；达到上限后栈申请失败，
     * Coroutine::Create 抛 std::runtime_error。 */
    size_t m_cfg_max_coroutine{65535};

    /* 调度器启动时预热的栈数量，默认 0 表示不预热。只预热「默认大小 + 默认保护属性」这一个桶，
     * 实际创建数取 min(本值, 栈池上限)，用于把首次借栈的 mmap/mprotect 开销提前到启动阶段。 */
    size_t m_cfg_static_coroutine{0};

    size_t m_cfg_stack_size{1024 * 12};         // 每个协程栈的请求字节数；按页对齐后才是可用容量
    bool m_cfg_stack_protect{true};             // 是否为栈加保护页（每栈多一页 + 一次 mprotect）
    size_t m_cfg_scan_interval_ms{1};           // 调度器维护拍间隔（毫秒）；0 表示不停拍
    size_t m_cfg_static_thread_num{0};          // 初始 worker 数；构造时取 hardware_concurrency()，不可用时取 1
    const size_t m_cfg_profile_printf_ms{1000}; // 打印profile间隔，0不打印

    /* Processer */
    /* 启动前配置，运行期模式切换由 Scheduler 控制，不能并发改这些字段。
     * auto 默认关闭，min/max=0 分别回退 initial，保持旧调用方的 worker 数。
     * 数量和 step 在 [1, INT_MAX]；initial 必须在解析后的 [min,max] 内。 */
    bool m_cfg_processer_auto_scale{false};
    size_t m_cfg_processer_min_num{0};
    size_t m_cfg_processer_max_num{0};
    /* interval 毫秒数在 [1, chrono::milliseconds::rep 最大值]；samples >= 1。 */
    size_t m_cfg_processer_scale_interval_ms{100};
    size_t m_cfg_processer_scale_up_samples{3};
    size_t m_cfg_processer_scale_down_samples{3};
    size_t m_cfg_processer_scale_step{1};
    /* busy 为 active worker 中正在 Resume 的百分比，0..100，down <= up。
     * 只作控制信号，不作为 worker 退役安全点，也不依赖 stall 告警开关。 */
    size_t m_cfg_processer_scale_up_busy_percent{80};
    size_t m_cfg_processer_scale_down_busy_percent{20};
    /* 全局 + active 本地 runnable 近似总数：up >= 1，0 <= down <= up。
     * >= up 或 busy >= up 连续命中扩容；<= down 且 busy <= down 连续命中缩容。 */
    size_t m_cfg_processer_scale_up_queue_watermark{1};
    size_t m_cfg_processer_scale_down_queue_watermark{0};

    const size_t m_cfg_processer_get_co_from_g_count{16};    // 每次从 global 队列批量取的上限
    const size_t m_cfg_processer_steal_once_min_task_num{8}; // 单次 steal 的最小数量，实际取 max(本值, 受害者队列/2)
    const size_t m_cfg_processer_worksteal_timeout_ms{10};   // 距上次 steal 不足该毫秒数时不再尝试
    const size_t m_cfg_processer_proc_interval_us{3000};     // 本地无任务时等待新工作的超时（微秒）

    /** CFS 相对服务权重，顺序 LOW/NORMAL/HIGH/CRITICAL；保留旧预算字段名及默认比例。
     *  记账为 elapsed_us / max(1, weight)，零值不再让任务永久失去调度机会。
     *  非线程安全，必须在启动前设置。 */
    std::array<uint64_t, CO_PRIORITY_COUNT> m_cfg_processer_priority_runtime_budget_us{50, 150, 200, 600};

    /* 一条 protected 栈要一次内存申请 + 一次 mprotect，所以复用栈；栈池支持动态缩容，
     * 下面几组参数控制池大小与调整节奏。 */

    /* 栈池中最大可以产生的数量。相当于程序中最多协程数量 */
    const size_t& m_cfg_stackpool_max_alloc_size{m_cfg_max_coroutine}; // 栈池中分配最大栈数量

    /* 全池保留数量下限：缩容目标取 max(本值, (rate+1)×平均借出数)，只回收空闲栈 */
    size_t m_cfg_stackpool_min_alloc_size{128};         // 栈池中最小栈数量
    const size_t m_cfg_stackpool_sample_interval{10};   // 缩容采样的间隔（毫秒）
    const size_t m_cfg_stackpool_adjust_interval{5000}; // 栈池动态调整的最小间隔（毫秒）

    /* 异常回调：设置后 Context 把捕获的异常文本（构造为 Errcode）交给它；未设置时改为
     * stderr 一行 + m_unhandled_exception_count 计数。回调必须不抛，抛出的异常被就地吞掉。 */
    ExceptionHandleCallback m_ext_coevent_exception_callback{nullptr};
    /* 未设置异常回调时的协程异常计数；与那行 stderr 日志配套，是默认隔离策略的可观测值 */
    std::atomic_uint64_t m_unhandled_exception_count{0};

    /* worker 无进展检测：>0 启用，协程单次执行超过该毫秒数即告警；
     * 0=关闭诊断采集（默认）；自动扩缩容的轻量 busy 信号独立维护。
     * 检测在调度线程 _FixTimingScan 内，每拍扫描。 */
    size_t m_cfg_worker_stall_warn_ms{0};
    /* 告警回调。未设置时默认 fprintf(stderr) 一行。调度线程调用，勿阻塞。 */
    WorkerStallCallback m_ext_worker_stall_callback{nullptr};

private:
    GlobalConfig();
};

} // namespace bbt::coroutine::detail