#pragma once
#include <memory>
#include <bbt/core/Attribute.hpp>
#include <bbt/coroutine/detail/interface/ICoroutine.hpp>
#include <bbt/coroutine/detail/Context.hpp>

namespace bbt::coroutine::detail
{

enum FollowEventStatus
{
    DEFAULT = 0,
    TIMEOUT = 1,
    READABLE = 2,
};

/**
 * @brief 协程对象
 * 
 * 原则上协程对象不允许多线程共享，目前协程对象只能通过全局队列来
 * 在多线程之间传递，这一方式是为了提高访问协程的效率。
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
    /* 手动挂起co并加入到全局队列中，和事件触发挂起无冲突，因为一个协程内逻辑串行 */
    virtual void                    YieldAndPushGCoQueue();

    virtual CoroutineId             GetId() override;
    CoroutineStatus                 GetStatus();
    int                             GetLastResumeEvent();

    /* 事件相关 */
    int                             YieldUntilTimeout(int ms);
    std::shared_ptr<CoPollEvent>    RegistCustom(int key);
    std::shared_ptr<CoPollEvent>    RegistCustom(int key, int timeout_ms);
    int                             YieldUntilFdReadable(int fd);
    int                             YieldUntilFdReadable(int fd, int timeout_ms);
    int                             YieldUntilFdWriteable(int fd);
    int                             YieldUntilFdWriteable(int fd, int timeout_ms);

protected:
    /* 事件被触发时回调 */
    void                            OnCoPollEvent(int event, int custom_key);

protected:
    static CoroutineId              GenCoroutineId();
    void                            _OnCoroutineFinal();
private:
    Context                         m_context;
    const CoroutineId               m_id{BBT_COROUTINE_INVALID_COROUTINE_ID};
    volatile CoroutineStatus        m_run_status{CoroutineStatus::CO_DEFAULT};

    FollowEventStatus               m_actived_event{DEFAULT}; // 触发的事件

    std::shared_ptr<CoPollEvent>    m_await_event{nullptr};
    std::mutex                      m_await_event_mutex;

    CoroutineFinalCallback          m_co_final_callback{nullptr};
    CoroutineOnYieldCallback        m_co_onyield_callback{nullptr};

    int                             m_last_resume_event{-1}; //最后一次导致此协程唤醒的事件
};

}