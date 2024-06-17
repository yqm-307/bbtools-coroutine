#pragma once
#include <bbt/coroutine/detail/Processer.hpp>

namespace bbt::coroutine::detail
{

class Scheduler
{
public:
    typedef std::unique_ptr<Scheduler> UPtr;
    ~Scheduler() {}

    static UPtr& GetInstance();

    /* 显示指定运行线程 */
    void Start(bool background_thread = false);
    void Stop();

    CoroutineId RegistCoroutineTask(const CoroutineCallback& handle);
    // void UnRegistCoroutineTask(CoroutineId coroutine_id);

protected:
    Scheduler() {}

    void _Run();
    /* 定时扫描 */
    void _FixTimingScan();
    /* 简单调度算法 */
    void _SampleSchuduleAlgorithm();
private:
    std::map<ProcesserId, Processer::SPtr>   m_processer_map;
    /* coroutine全局队列 */
    CoroutineQueue                      m_global_coroutine_deque;
    volatile bool                       m_is_running{true};

    /* XXX 配置，先静态配置 */
    const size_t                        m_cfg_stack_size{1024 * 8};
    const size_t                        m_cfg_scan_interval_ms{50};
    const bool                          m_cfg_static_thread{true};
    const size_t                        m_cfg_static_thread_num{2};
};

}