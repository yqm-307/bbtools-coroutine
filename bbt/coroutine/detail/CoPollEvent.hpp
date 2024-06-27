#pragma once
#include <memory>
#include <bbt/base/Attribute.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/interface/IPollEvent.hpp>

namespace bbt::coroutine::detail
{


class CoPollEvent:
    public IPollEvent,
    public std::enable_shared_from_this<CoPollEvent>
{
public:
    friend class CoPoller;
    typedef std::shared_ptr<CoPollEvent> SPtr;

    static SPtr                     Create(std::shared_ptr<Coroutine> coroutine, const CoPollEventCallback& cb);

    BBTATTR_FUNC_Ctor_Hidden        CoPollEvent(std::shared_ptr<Coroutine> coroutine, const CoPollEventCallback& cb);
                                    ~CoPollEvent();

    virtual int                     GetEvent() const override;
    epoll_event*                    GetEpollEvent(int fd);
    bool                            IsListening() const;
    bool                            IsFinal() const;
    CoPollEventStatus               GetStatus() const;

    virtual void                    Trigger(IPoller* poller, int trigger_events) override;
    /* 下面3个函数注册不同的事件，但是事件在第一次触发后失效 */
    int                             InitFdReadableEvent(int fd);
    int                             InitTimeoutEvent(int timeout);
    int                             InitCustomEvent(int key, void* args);
    int                             Regist();

    int                             UnRegistEvent();

    /* 这个类用来包裹对象的智能指针，防止事件被意外被释放 */
    struct PrivData {std::shared_ptr<IPollEvent> event_sptr{nullptr};};
protected:
    int                             _RegistCustomEvent();
    int                             _RegistTimeoutEvent();
    int                             _RegistFdEvent();
    int                             _CannelAllFdEvent();

    int                             _CreateEpollEvent(int fd, int epoll_events);
    void                            _DestoryEpollEvent(int fd);

    void                            _OnListen();
    void                            _OnFinal();
private:
    std::shared_ptr<Coroutine>      m_coroutine{nullptr};
    int                             m_fd{-1};
    int                             m_timerfd{-1};
    PollEventType                   m_type{PollEventType::POLL_EVENT_DEFAULT};
    int                             m_timeout{-1};
    bool                            m_has_custom_event{false};
    int                             m_custom_key{-1};
    /* 事件对象内部保存注册到epoll时的结构体，自己管理生命期更安全 */
    std::unordered_map<int, epoll_event>
                                    m_epoll_event_map;
    std::mutex                      m_epoll_event_map_mutex;

    CoPollEventCallback             m_onevent_callback{nullptr};
    volatile CoPollEventStatus      m_run_status{CoPollEventStatus::POLLEVENT_DEFAULT};
};

}