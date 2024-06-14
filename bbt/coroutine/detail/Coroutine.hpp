#include <bbt/coroutine/detail/ICoroutine.hpp>
#include <bbt/coroutine/detail/Context.hpp>

namespace bbt::coroutine::detail
{

class Coroutine:
    public ICoroutine
{
public:
    Coroutine(int stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool need_protect = true);
    ~Coroutine();
    
    virtual void        Resume() override;
    virtual void        Yield() override;
    virtual CoroutineId GetId() override;
    CoroutineStatus     GetStatus();

protected:
    static CoroutineId  GenCoroutineId();
private:
    Context             m_context;
    const CoroutineId   m_id{BBT_COROUTINE_INVALID_COROUTINE_ID};
    CoroutineStatus     m_run_status{CoroutineStatus::Default};
};

}