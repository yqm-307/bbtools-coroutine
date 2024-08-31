#pragma once
#include <memory>
#include <bbt/base/Attribute.hpp>
#include <bbt/coroutine/detail/interface/ICoroutine.hpp>
#include <bbt/coroutine/detail/Context.hpp>

namespace bbt::coroutine::detail
{

/**
 * @brief 协程对象
 * 
 */
class Coroutine:
    public ICoroutine,
    public std::enable_shared_from_this<Coroutine>
{
public:
    typedef std::shared_ptr<Coroutine> SPtr;

    BBTATTR_FUNC_Ctor_Hidden
    Coroutine(int stack_size, const CoroutineCallback& co_func, bool need_protect);
    BBTATTR_FUNC_Ctor_Hidden
    Coroutine(int stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool need_protect);
    ~Coroutine();
    
    static SPtr                     Create(int stack_size, const CoroutineCallback& co_func, bool need_protect = true);
    static SPtr                     Create(int stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool need_protect = true);

    virtual void                    Resume() override;
    virtual void                    Yield() override;
    virtual int                     YieldWithCallback(const CoroutineOnYieldCallback& cb) override;

    virtual CoroutineId             GetId() override;
    CoroutineStatus                 GetStatus();
    int                             GetLastResumeEvent();

protected:
    /* 事件被触发时回调 */
    // void                            OnCoPollEvent(int event, int custom_key);

protected:
    static CoroutineId              GenCoroutineId();
    void                            _OnCoroutineFinal();
private:
    Context                         m_context;
    const CoroutineId               m_id{BBT_COROUTINE_INVALID_COROUTINE_ID};
    volatile CoroutineStatus        m_run_status{CoroutineStatus::CO_DEFAULT};

    CoroutineFinalCallback          m_co_final_callback{nullptr};
    int                             m_last_resume_event{-1}; //最后一次导致此协程唤醒的事件
};

}