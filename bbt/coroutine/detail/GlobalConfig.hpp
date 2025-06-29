#include <memory>


namespace bbt::coroutine::detail
{

/**
 * @brief 协程库静态配置
 * 
 */
class GlobalConfig
{
public:
    typedef std::unique_ptr<GlobalConfig> UPtr;
    static UPtr& GetInstance() {
        static UPtr _inst{nullptr};
        if (_inst == nullptr)
            _inst = UPtr(new GlobalConfig());
        return _inst;
    }


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
    size_t                                      m_cfg_static_thread_num{0};                 // 静态线程数，如果不指定则默认是 std::thread::hardware_concurrency()
    const size_t                                m_cfg_profile_printf_ms{1000};              // 打印profile间隔，0不打印

    /* Processer */
    const size_t                                m_cfg_processer_get_co_from_g_count{16};   // 每次从g队列获取任务数量
    const size_t                                m_cfg_processer_steal_once_min_task_num{8};// 如果有任务，最少偷64个
    const size_t                                m_cfg_processer_worksteal_timeout_ms{10};   // work steal 认为任务饿死的超时时间
    const size_t                                m_cfg_processer_proc_interval_us{3000};     // proc 每次执行间隔时间

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

private:
    GlobalConfig();
};

} // namespace bbt::coroutine::detail