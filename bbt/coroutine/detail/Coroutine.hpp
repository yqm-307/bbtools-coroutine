#pragma once
#include <memory>
#include <bbt/base/Attribute.hpp>
#include <bbt/coroutine/detail/interface/ICoroutine.hpp>
#include <bbt/coroutine/detail/Context.hpp>

namespace bbt::coroutine::detail
{

class Coroutine:
    public ICoroutine,
    public std::enable_shared_from_this<Coroutine>
{
public:
    friend class Processer;
    typedef std::shared_ptr<Coroutine> SPtr;

    BBTATTR_FUNC_Ctor_Hidden
    Coroutine(int stack_size, const CoroutineCallback& co_func, bool need_protect = true);
    ~Coroutine();
    
    static SPtr                     Create(int stack_size, const CoroutineCallback& co_func, bool need_protect = true);
    virtual void                    Resume() override;
    virtual void                    Yield() override;
    virtual CoroutineId             GetId() override;
    CoroutineStatus                 GetStatus();
    void                            OnEventTimeout();
protected:
    void                            BindProcesser(std::shared_ptr<Processer> processer);

protected:
    static CoroutineId              GenCoroutineId();
    void                            _OnCoroutineFinal();
    void                            _OnBindWithProcesser(ProcesserId id);
private:
    ProcesserId                     m_bind_processer_id{BBT_COROUTINE_INVALID_PROCESSER_ID};

    Context                         m_context;
    const CoroutineId               m_id{BBT_COROUTINE_INVALID_COROUTINE_ID};
    volatile CoroutineStatus        m_run_status{CoroutineStatus::CO_DEFAULT};
};

}