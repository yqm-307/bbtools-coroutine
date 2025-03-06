#pragma once
#include <memory>
#include <bbt/core/Attribute.hpp>
#include <bbt/pollevent/Event.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/interface/IPollEvent.hpp>

namespace bbt::coroutine::detail
{


/**
 * @brief 协程轮询事件（无锁）
 * 
 * 辅助协程实现挂起和唤醒
 */
class CoPollEvent:
    public std::enable_shared_from_this<CoPollEvent>
{
public:
    friend class CoPoller;
    typedef std::shared_ptr<CoPollEvent> SPtr;

    static SPtr                     Create(CoroutineId id, const CoPollEventCallback& cb);

    BBTATTR_FUNC_Ctor_Hidden        CoPollEvent(CoroutineId id, const CoPollEventCallback& cb);
                                    ~CoPollEvent();

    int                             GetEvent() const;
    bool                            IsListening() const;
    bool                            IsFinal() const;
    CoPollEventStatus               GetStatus() const;
    CoPollEventId                   GetId() const;
    int                             GetFd() const;
    int64_t                         GetTimeout() const;

    int                             Trigger(short trigger_events);
    /* 初始化后调用Regist注册事件 */
    int                             InitFdEvent(int fd, short events, int timeout);
    int                             InitCustomEvent(int key, void* args);
    int                             Regist();
    int                             UnRegist();

protected:
    // int                             _RegistCustomEvent();
    int                             _RegistFdEvent();
    int                             _CannelAllFdEvent();

    void                            _OnListen();
    void                            _OnFinal();
    static CoPollEventId            _GenerateId();
private:
    CoroutineId                     m_co_id{0};
    std::shared_ptr<bbt::pollevent::Event>
                                    m_event{nullptr};
    int                             m_timeout{-1};
    bool                            m_has_custom_event{false};
    int                             m_custom_key{-1};
    CoPollEventId                   m_event_id{BBT_COROUTINE_INVALID_COPOLLEVENT_ID};

    CoPollEventCallback             m_onevent_callback{nullptr};
    CoPollEventStatus               m_run_status{CoPollEventStatus::POLLEVENT_DEFAULT};
    std::mutex                      m_onevent_callback_mtx;
};

}