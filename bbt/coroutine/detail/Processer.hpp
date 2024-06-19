#pragma once
#include <condition_variable>
#include <bbt/coroutine/detail/CoroutineQueue.hpp>

namespace bbt::coroutine::detail
{

class Processer:
    public std::enable_shared_from_this<Processer>
{
public:
    friend class Scheduler;
    typedef std::shared_ptr<Processer> SPtr;

    static SPtr Create();
    /* 获取当前线程Processer */
    static SPtr GetLocalProcesser(); 

    explicit
    Processer();
    ~Processer();

    /* 获取processer当前状态 */
    ProcesserStatus                 GetStatus();
    /* 获取processer id */
    ProcesserId                     GetId();
    Coroutine::SPtr                 GetCurrentCoroutine();

    /**
     * 挂起事件相关
     */
    int                             RegistTimeoutEvent(int ms);                 

protected:
    /* 非公开库内部接口 */
    void                            Start(bool background_thread = true);
    void                            Stop();
    void                            Notify();
    int                             GetLoadValue();
    int                             GetExecutableNum(); /* 可执行协程数 */
    void                            AddCoroutineTask(Coroutine::SPtr coroutine);
    void                            AddCoroutineTaskRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end);

    void                            AddActiveCoroutine(Coroutine::SPtr actived_coroutine);
    void                            AddActiveCoroutine(std::vector<Coroutine::SPtr> coroutines);
protected:
    static ProcesserId              _GenProcesserId();
    void                            _OnAddCorotinue();
    void                            _Run();
private:
    const ProcesserId               m_id{BBT_COROUTINE_INVALID_PROCESSER_ID};
    volatile ProcesserStatus        m_run_status{ProcesserStatus::PROC_DEFAULT};
    CoroutineQueue                  m_coroutine_queue;
    CoroutineQueue                  m_actived_queue;    // 被激活的协程

    std::condition_variable         m_run_cond;
    std::mutex                      m_run_cond_mutex;

    volatile bool                   m_is_running{true};

    Coroutine::SPtr                 m_running_coroutine{nullptr};   // processer当前运行中的协程
};

}