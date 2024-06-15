// #include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/CoroutineQueue.hpp>

namespace bbt::coroutine::detail
{

class Scheduler
{
public:
    Scheduler();
    ~Scheduler();

    /* 显示指定运行线程 */
    void Start();
    void Stop();

    CoroutineId RegistCoroutineTask(const CoroutineCallback& handle);
    // void UnRegistCoroutineTask(CoroutineId coroutine_id);

protected:
    /* 定时扫描 */
    void _FixTimingScan();

    void _SampleSchuduleAlgorithm();

    /* 协程结束后，scheduler执行回收、记录操作 */
    void _OnCoroutineFinal();
private:
    std::map<ProcesserId, Processer*>   m_processer_map;
    /* coroutine全局队列 */
    CoroutineQueue                      m_global_coroutine_deque;
    volatile bool                       m_is_running{true};

    /* XXX 配置，先静态配置 */
    const size_t                        m_cfg_stack_size{1024 * 8};
    const size_t                        m_cfg_scan_interval_ms{50};
    const bool                          m_cfg_static_thread{true};
    const size_t                        m_cfg_static_thread_num{3};
};

}