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
    const size_t                                m_cfg_stack_size{1024 * 8};
    const size_t                                m_cfg_scan_interval_ms{10};
    const bool                                  m_cfg_static_thread{true};
    const size_t                                m_cfg_static_thread_num{8};
    const size_t                                m_cfg_profile_printf_ms{5000}; // 打印profile间隔，0不打印

    const size_t                                m_cfg_processer_get_co_from_g_count{256};  // 每次从g队列获取任务数量

};

} // namespace bbt::coroutine::detail