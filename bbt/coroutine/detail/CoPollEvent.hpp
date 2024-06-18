#pragma once
#include <memory>
#include <bbt/base/Attribute.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/interface/IPollEvent.hpp>

namespace bbt::coroutine::detail
{

enum PollEventType
{
    POLL_EVENT_DEFAULT      = 0,
    POLL_EVENT_TIMEOUT      = 1,
    POLL_EVENT_WRITEABLE    = 2,
    POLL_EVENT_READABLE     = 3,
};

class CoPollEvent:
    public IPollEvent,
    public std::enable_shared_from_this<CoPollEvent>
{
public:
    typedef std::shared_ptr<CoPollEvent> SPtr;

    /* 创建一个超时事件 */
    static SPtr                     Create(std::shared_ptr<Coroutine> coroutine, int timeout, const CoPollEventCallback& cb);

    BBTATTR_FUNC_Ctor_Hidden        CoPollEvent(std::shared_ptr<Coroutine> coroutine, PollEventType type, int timeout, const CoPollEventCallback& cb);
                                    ~CoPollEvent();

    /* 轮询事件，关注的套接字 */
    virtual int                     GetFd() override;
    /* 触发监听事件 */
    virtual void                    Trigger(IPoller* poller, int trigger_events) override;
    /* 获取监听的事件 */
    virtual int                     GetEvent() override;
    /* 注册监听事件 */
    int                             RegistEvent();
    /* 获取epoll_event结构体 */
    epoll_event&                    GetEpollEvent();


protected:

    int                             _RegistTimeoutEvent();
private:
    std::shared_ptr<Coroutine>      m_coroutine{nullptr};
    int                             m_fd{-1};
    int                             m_events{-1};
    PollEventType                   m_type{PollEventType::POLL_EVENT_DEFAULT};
    int                             m_timeout{-1};
    /* 事件对象内部保存注册到epoll时的结构体，自己管理生命期更安全 */
    epoll_event                     m_epoll_event;

    CoPollEventCallback             m_onevent_callback{nullptr};
};

}