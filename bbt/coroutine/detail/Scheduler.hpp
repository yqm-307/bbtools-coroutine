#pragma once
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/base/thread/Lock.hpp>

namespace bbt::coroutine::detail
{

class Scheduler
{
public:
    friend class Profiler;
    typedef std::unique_ptr<Scheduler> UPtr;
    ~Scheduler();

    static UPtr& GetInstance();

    /* 显示指定运行线程 */
    void Start(bool background_thread = false);
    void Stop();

    CoroutineId                                 RegistCoroutineTask(const CoroutineCallback& handle);
    /* 协程被激活，重新加入全局队列 */
    void                                        OnActiveCoroutine(Coroutine::SPtr coroutine);
    /* 从全局队列中取一定数量的协程 */
    size_t                                      GetGlobalCoroutine(std::vector<Coroutine::SPtr>& coroutines, size_t size);
    // size_t                                      GetGlobalCoroutine(CoroutineQueue& coroutines, size_t size);
protected:
    Scheduler();
    void                                        _Init();

    void                                        _Run();
    /* 定时扫描 */
    void                                        _FixTimingScan();
    /* 简单调度算法 */
    void                                        _SampleSchuduleAlgorithm();

    void                                        _CreateProcessers();
    void                                        _DestoryProcessers();

    bool                                        _LoadBlance2Proc(Coroutine::SPtr co);
private:
    /* Scheduler */
    bbt::clock::Timestamp<>                     m_begin_timestamp;  // 调度器开启时间
    std::thread*                                m_thread{nullptr};
    std::vector<std::thread*>                   m_proc_threads;

    /* Processer 管理 */
    std::map<ProcesserId, Processer::SPtr>      m_processer_map;
    std::vector<Processer::SPtr>                m_load_blance_vec;
    uint32_t                                    m_load_idx{0};
    std::mutex                                  m_processer_map_mutex;
    bbt::thread::CountDownLatch                 m_down_latch;

    /* coroutine全局队列 */
    CoroutineQueue                              m_global_coroutine_deque;
    volatile bool                               m_is_running{true};
    volatile ScheudlerStatus                    m_run_status{ScheudlerStatus::SCHE_DEFAULT};

    uint64_t                                    m_regist_coroutine_count{0};
};

}