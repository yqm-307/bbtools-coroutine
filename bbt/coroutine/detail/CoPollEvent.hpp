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
    static SPtr                     Create(std::shared_ptr<Coroutine> coroutine, int timeout);

    BBTATTR_FUNC_Ctor_Hidden        CoPollEvent(std::shared_ptr<Coroutine> coroutine, PollEventType type, int timeout);
                                    ~CoPollEvent();

    virtual int                     GetFd() override;
    virtual void                    Trigger(IPoller* poller, int trigger_events) override;
    virtual int                     GetEvent() override;
    int                             RegistEvent();
    epoll_event&                    GetEpollEvent();


protected:

    int                             _RegistTimeoutEvent();
private:
    std::shared_ptr<Coroutine>      m_coroutine{nullptr};
    int                             m_fd{-1};
    int                             m_events{-1};
    PollEventType                   m_type{PollEventType::POLL_EVENT_DEFAULT};
    int                             m_timeout{-1};
    epoll_event                     m_epoll_event;
};

}