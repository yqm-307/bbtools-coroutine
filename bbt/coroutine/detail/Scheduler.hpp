#pragma once
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
 */
class Scheduler
{
public:
    friend class Profiler;
    friend class Processer;
    typedef std::unique_ptr<Scheduler> UPtr;
    ~Scheduler();

    static UPtr& GetInstance();

    /* 显示指定运行线程 */
    void                                        Start(SchedulerStartOpt opt = SCHE_START_OPT_SCHE_THREAD);
    void                                        Stop();
    void                                        LoopOnce();

    void                                        RegistCoroutineTask(const CoroutineCallback& handle);
    void                                        RegistCoroutineTask(const CoroutineCallback& handle, bool& succ) noexcept;
    /* 协程被激活，重新加入全局队列 */
    void                                        OnActiveCoroutine(CoroutinePriority priority, Coroutine::Ptr coroutine);

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

    void                                        _Run();
    /* 定时扫描 */
    void                                        _FixTimingScan();
    void                                        _CreateProcessers();
    void                                        _DestoryProcessers();

    bool                                        _LoadBlance2Proc(CoroutinePriority priority, Coroutine::Ptr co);
    /* 初始化全局实例 */
    void                                        _InitGlobalUniqInstance();

    void                                        _OnUpdate();
private:
    /* Scheduler */
    bbt::core::clock::Timestamp<>               m_begin_timestamp;  // 调度器开启时间
    std::thread*                                m_sche_thread{nullptr};
    std::vector<std::thread*>                   m_proc_threads;

    /* Processer 管理 */
    std::map<ProcesserId, Processer::SPtr>      m_processer_map;
    std::vector<Processer::SPtr>                m_load_blance_vec;
    uint32_t                                    m_load_idx{0};
    uint32_t                                    m_steal_idx{0};
    std::mutex                                  m_processer_map_mutex;
    bbt::core::thread::CountDownLatch           m_down_latch;

    /* coroutine全局队列 */
    CoPriorityQueue                             m_global_coroutine_queue;
    volatile bool                               m_is_running{true};
    volatile ScheudlerStatus                    m_run_status{ScheudlerStatus::SCHE_DEFAULT};

    uint64_t                                    m_regist_coroutine_count{0};
};

}