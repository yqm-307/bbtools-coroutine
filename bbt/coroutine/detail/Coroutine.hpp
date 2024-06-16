#pragma once
#include <memory>
#include <bbt/base/Attribute.hpp>
#include <bbt/coroutine/detail/ICoroutine.hpp>
#include <bbt/coroutine/detail/Context.hpp>

namespace bbt::coroutine::detail
{

class Coroutine:
    public ICoroutine,
    public std::enable_shared_from_this<Coroutine>
{
public:
    typedef std::shared_ptr<Coroutine> SPtr;

    BBTATTR_FUNC_Ctor_Hidden
    Coroutine(int stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool need_protect = true);
    ~Coroutine();
    
    static SPtr                     Create(int stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool need_protect = true);
    virtual void                    Resume() override;
    virtual void                    Yield() override;
    virtual CoroutineId             GetId() override;
    CoroutineStatus                 GetStatus();

protected:
    static CoroutineId              GenCoroutineId();
private:
    Context                         m_context;
    const CoroutineId               m_id{BBT_COROUTINE_INVALID_COROUTINE_ID};
    volatile CoroutineStatus        m_run_status{CoroutineStatus::CO_DEFAULT};
};

}