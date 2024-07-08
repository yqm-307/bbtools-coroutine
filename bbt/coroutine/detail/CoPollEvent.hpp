#pragma once
#include <memory>
#include <bbt/base/Attribute.hpp>
#include <bbt/pollevent/Event.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/interface/IPollEvent.hpp>

namespace bbt::coroutine::detail
{


/**
 * @brief 协程轮询事件
 * 
 * 辅助协程实现挂起和唤醒
 */
class CoPollEvent:
    // public IPollEvent,
    public std::enable_shared_from_this<CoPollEvent>
{
public:
    friend class CoPoller;
    typedef std::shared_ptr<CoPollEvent> SPtr;

    static SPtr                     Create(std::shared_ptr<Coroutine> coroutine, const CoPollEventCallback& cb);

    BBTATTR_FUNC_Ctor_Hidden        CoPollEvent(std::shared_ptr<Coroutine> coroutine, const CoPollEventCallback& cb);
                                    ~CoPollEvent();

    // int                             GetEvent() const;
    // epoll_event*                    GetEpollEvent(int fd);
    bool                            IsListening() const;
    bool                            IsFinal() const;
    CoPollEventStatus               GetStatus() const;

    void                            Trigger(short trigger_events);
    /* 下面3个函数注册不同的事件，但是事件在第一次触发后失效 */

    int                             RegistFdEvent(int fd, short events, int timeout);
    int                             InitCustomEvent(int key, void* args);
    int                             Regist();

    int                             UnRegistEvent();

    /* 这个类用来包裹对象的智能指针，防止事件被意外被释放 */
    struct PrivData {std::shared_ptr<IPollEvent> event_sptr{nullptr};};
protected:
    int                             _RegistCustomEvent();
    int                             _RegistFdEvent();
    int                             _CannelAllFdEvent();

    void                            _OnListen();
    void                            _OnFinal();
private:
    std::shared_ptr<Coroutine>      m_coroutine{nullptr};
    std::shared_ptr<bbt::pollevent::Event>
                                    m_event{nullptr};
    // int                             m_type{PollEventType::POLL_EVENT_DEFAULT};
    int                             m_timeout{-1};
    bool                            m_has_custom_event{false};
    int                             m_custom_key{-1};

    CoPollEventCallback             m_onevent_callback{nullptr};
    volatile CoPollEventStatus      m_run_status{CoPollEventStatus::POLLEVENT_DEFAULT};
};

}