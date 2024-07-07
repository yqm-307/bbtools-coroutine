#pragma once
#include <memory>
#include <bbt/base/Attribute.hpp>
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
 */
class Coroutine:
    public ICoroutine,
    public std::enable_shared_from_this<Coroutine>
{
public:
    friend class Processer;
    friend class sync::CoCond;
    friend int Hook_Sleep(int);
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
    virtual CoroutineId             GetId() override;
    CoroutineStatus                 GetStatus();

    std::shared_ptr<CoPollEvent>    RegistTimeout(int ms);
    std::shared_ptr<CoPollEvent>    RegistCustom(int key);
    std::shared_ptr<CoPollEvent>    RegistCustom(int key, int timeout_ms);
    std::shared_ptr<CoPollEvent>    RegistFdReadable(int fd);
    std::shared_ptr<CoPollEvent>    RegistFdReadable(int fd, int timeout_ms);
    std::shared_ptr<CoPollEvent>    RegistFdWriteable(int fd);
    std::shared_ptr<CoPollEvent>    RegistFdWriteable(int fd, int timeout_ms);

protected:
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
};

}