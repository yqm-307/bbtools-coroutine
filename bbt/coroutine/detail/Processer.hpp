#pragma once
#include <condition_variable>
#include <bbt/coroutine/detail/CoroutineQueue.hpp>

namespace bbt::coroutine::detail
{

class Processer:
    public std::enable_shared_from_this<Processer>
{
public:
    typedef std::shared_ptr<Processer> SPtr;

    static SPtr Create();

    explicit
    Processer();
    ~Processer();

    ProcesserId                     GetProcesserId();
    ProcesserStatus                 GetStatus();
    /* 获取负载值 */
    int                             GetLoadValue();
    void                            AddCoroutineTask(Coroutine::SPtr coroutine);
    void                            AddCoroutineTaskRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end);
    void                            Start(bool background_thread = true);
    void                            Stop();

    void                            OnAddCorotinue();
protected:
    static ProcesserId              _GenProcesserId();
    void                            _Run();
private:
    const ProcesserId               m_id{BBT_COROUTINE_INVALID_PROCESSER_ID};
    volatile ProcesserStatus        m_run_status{ProcesserStatus::PROC_Default};
    CoroutineQueue                  m_coroutine_queue;
    std::map<CoroutineId, Coroutine::SPtr>
                                    m_wait_coroutine_map;

    std::condition_variable         m_run_cond;
    std::mutex                      m_run_cond_mutex;

    volatile bool                   m_is_running{true};
};

}