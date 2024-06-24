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

class Coroutine:
    public ICoroutine,
    public std::enable_shared_from_this<Coroutine>
{
public:
    friend class Processer;
    friend class sync::CoCond;
    friend class sync::Chan;
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
    std::shared_ptr<CoPollEvent>    RegistReadable(int fd, int ms);
    std::shared_ptr<CoPollEvent>    RegistReadableET(int fd, int ms);   // 边缘触发

protected:
    void                            OnEventTimeout(std::shared_ptr<CoPollEvent> event);
    void                            OnEventReadable(std::shared_ptr<CoPollEvent> evnet);
    void                            OnEventChanWrite();

protected:
    static CoroutineId              GenCoroutineId();
    void                            _OnCoroutineFinal();
    void                            _OnEventFinal(); // 事件触发结束
    std::shared_ptr<CoPollEvent>    _RegistReadableEx(int fd, int ms, int ext_event);
private:
    // ProcesserId                     m_bind_processer_id{BBT_COROUTINE_INVALID_PROCESSER_ID};

    Context                         m_context;
    const CoroutineId               m_id{BBT_COROUTINE_INVALID_COROUTINE_ID};
    volatile CoroutineStatus        m_run_status{CoroutineStatus::CO_DEFAULT};

    FollowEventStatus               m_actived_event{DEFAULT}; // 触发的事件

    std::shared_ptr<CoPollEvent>    m_timeout_event{nullptr};
    std::shared_ptr<CoPollEvent>    m_readable_event{nullptr};

    CoroutineFinalCallback          m_co_final_callback{nullptr};
};

}