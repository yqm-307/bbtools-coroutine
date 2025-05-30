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


    /* XXX 配置，先静态配置 */
    const size_t                                m_cfg_stack_size{1024 * 8};                 // 栈大小
    const bool                                  m_cfg_stack_protect{true};                  // 栈保护
    const size_t                                m_cfg_scan_interval_ms{1};                  // scheduler 扫描间隔
    const bool                                  m_cfg_static_thread{true};                  // 是否静态创建线程 TODO 动态线程
    size_t                                      m_cfg_static_thread_num{0};                 // 静态线程数，如果不指定则默认是 std::thread::hardware_concurrency()
    const size_t                                m_cfg_profile_printf_ms{1000};              // 打印profile间隔，0不打印

    /* Processer */
    const size_t                                m_cfg_processer_get_co_from_g_count{16};   // 每次从g队列获取任务数量
    const size_t                                m_cfg_processer_steal_once_min_task_num{8};// 如果有任务，最少偷64个
    const size_t                                m_cfg_processer_worksteal_timeout_ms{10};   // work steal 认为任务饿死的超时时间
    const size_t                                m_cfg_processer_proc_interval_us{3000};     // proc 每次执行间隔时间

    /* 栈池配置 */
    const size_t                                m_cfg_stackpool_max_alloc_size{1024 * 1000}; // 栈池中分配最大栈数量
    const size_t                                m_cfg_stackpool_min_alloc_size{1024};       // 栈池中最小栈数量
    const size_t                                m_cfg_stackpool_sample_interval{10};        // 采样间隔
    const size_t                                m_cfg_stackpool_adjust_interval{5000};      // 栈池进行动态调整间隔

private:
    GlobalConfig();
};

} // namespace bbt::coroutine::detail