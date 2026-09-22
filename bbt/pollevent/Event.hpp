#pragma once
#include <bbt/pollevent/detail/Define.hpp>
#include <bbt/pollevent/detail/EventBase.hpp>
#include <boost/asio.hpp>
#include <memory>

namespace bbt::pollevent
{

class Event:
    public std::enable_shared_from_this<Event>
{
public:
    typedef std::shared_ptr<Event> SPtr;

    Event(detail::EventBase* base, evutil_socket_t fd, short listen_events,
          const OnEventCallback& onevent_cb);
    ~Event();

    int                         StartListen(uint64_t timeout);
    int                         CancelListen(bool need_close_fd = false);
    int                         GetSocket() const;
    short                       GetEvents() const;
    EventId                     GetEventId();
    int64_t                     GetTimeoutMs() const;

    /* 自定义事件触发 */
    int                         Trigger(int flag);

private:
    EventId                     GenerateId();

    friend class EventLoop;
    static void AddEventCallback(EventId id, const OnEventCallback& cb);
    static void DelEventCallback(EventId id);
    static void CallEventCallback(EventId id, int fd, short events);

    void                        DoAsyncWait(short event_flag);

private:
    EventId                     m_id{0};
    detail::EventBase*          m_ref_base{nullptr};
    evutil_socket_t             m_fd{-1};
    short                       m_listen_events{0};
    int64_t                     m_timeout{-1};

    /* ASIO 对象 — 二选一 */
    std::unique_ptr<boost::asio::posix::stream_descriptor> m_sd;
    std::unique_ptr<boost::asio::steady_timer>             m_timer;

    static std::unordered_map<EventId, OnEventCallback>    m_callback_map;
    static std::mutex                                      m_callback_map_mtx;
};

} // namespace bbt::pollevent
