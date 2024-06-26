#include <memory>


namespace bbt::coroutine::detail
{

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
    const size_t                                m_cfg_stack_size{1024 * 4};                 // 栈大小
    const bool                                  m_cfg_stack_protect{true};                  // 栈保护
    const size_t                                m_cfg_scan_interval_ms{1};                  // scheduler 扫描间隔
    const bool                                  m_cfg_static_thread{true};                  // 是否静态创建线程
    const size_t                                m_cfg_static_thread_num{2};                 // 静态线程数
    const size_t                                m_cfg_profile_printf_ms{1000};              // 打印profile间隔，0不打印

    const size_t                                m_cfg_processer_get_co_from_g_count{256};   // 每次从g队列获取任务数量

    /* 栈池配置 */
    const size_t                                m_cfg_stackpool_max_alloc_size{1024 * 100};      // 栈池中分配最大栈数量
    const size_t                                m_cfg_stackpool_min_alloc_size{1024};       // 栈池中最小栈数量
    const size_t                                m_cfg_stackpool_sample_interval{100};        // 采样间隔
};

} // namespace bbt::coroutine::detail