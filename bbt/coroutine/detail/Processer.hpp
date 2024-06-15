#include <bbt/coroutine/detail/CoroutineQueue.hpp>

namespace bbt::coroutine::detail
{

class Processer
{
public:
    explicit
    Processer();
    ~Processer();

    ProcesserStatus     GetStatus();
    /* 获取负载值 */
    int                 GetLoadValue();
protected:
    static ProcesserId  _GenProcesserId();
private:
    const ProcesserId   m_id{BBT_COROUTINE_INVALID_PROCESSER_ID};
    volatile ProcesserStatus m_run_status{ProcesserStatus::Default};
    CoroutineQueue      m_coroutine_queue;
};

}