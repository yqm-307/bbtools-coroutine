#include <atomic>
#include <boost/asio.hpp>
#include <bbt/pollevent/Event.hpp>
#include <bbt/pollevent/detail/EventBase.hpp>
#include <bbt/core/clock/Clock.hpp>

namespace bbt::pollevent
{

std::unordered_map<EventId, OnEventCallback>    Event::m_callback_map;
std::mutex                                      Event::m_callback_map_mtx;

Event::Event(detail::EventBase* base, evutil_socket_t fd, short listen_events,
             const OnEventCallback& onevent_cb)
    :m_id(GenerateId()),
     m_ref_base(base),
     m_fd(fd),
     m_listen_events(listen_events)
{
    Assert(base != nullptr);
    AddEventCallback(m_id, onevent_cb);

    // 判断事件类型并创建 ASIO 对象（fd 和 timer 可共存）
    bool is_fd_event = (fd >= 0) && (listen_events & (EV_READ | EV_WRITE | EV_CLOSED));
    bool is_timer   = (listen_events & EV_TIMEOUT);

    if (is_fd_event) {
        Assert(fd >= 0);
        m_sd = std::make_unique<boost::asio::posix::stream_descriptor>(
            base->GetContext(), fd);
    }
    if (is_timer) {
        m_timer = std::make_unique<boost::asio::steady_timer>(base->GetContext());
    }

    m_ref_base->IncEventCount();
}

Event::~Event()
{
    CancelListen();
    m_ref_base->DecEventCount();
}

int Event::StartListen(uint64_t timeout)
{
    // 记录超时
    if (timeout > 0) {
        m_timeout = bbt::core::clock::nowAfter(
            bbt::core::clock::milliseconds(timeout + 1))
            .time_since_epoch().count();
    } else {
        m_timeout = -1;
    }

    // fd 事件
    if (m_sd) {
        boost::system::error_code ec;
        m_sd->cancel(ec);

        if (m_listen_events & EV_READ)
            DoAsyncWait(EV_READ);
        if (m_listen_events & EV_WRITE)
            DoAsyncWait(EV_WRITE);
    }

    // 定时器事件（可与 fd 事件共存）
    if (m_timer) {
        if (timeout > 0) {
            m_timer->cancel();  // 防止重复 StartListen 导致双重 async_wait UB
            m_timer->expires_after(std::chrono::milliseconds(timeout));
            m_timer->async_wait([this](boost::system::error_code ec) {
                if (ec == boost::asio::error::operation_aborted)
                    return;
                // Plan C: timer 触发时取消 fd 等待，防止 double-fire
                if (m_sd) {
                    boost::system::error_code ignored;
                    m_sd->cancel(ignored);
                }
                CallEventCallback(m_id, -1, EV_TIMEOUT);
            });
        }
        return 0;
    }

    // 自定义事件：无 fd 无 timer，由 Trigger() 驱动
    if (timeout > 0 && m_fd < 0) {
        // 为自定义事件也支持超时
        m_timer = std::make_unique<boost::asio::steady_timer>(
            m_ref_base->GetContext());
        m_timer->expires_after(std::chrono::milliseconds(timeout));
        m_timer->async_wait([this](boost::system::error_code ec) {
            if (ec == boost::asio::error::operation_aborted)
                return;
            CallEventCallback(m_id, -1, EV_TIMEOUT);
        });
        return 0;
    }

    return 0;
}

void Event::DoAsyncWait(short event_flag)
{
    if (!m_sd) return;

    auto wait_type = (event_flag == EV_READ)
        ? boost::asio::posix::stream_descriptor::wait_read
        : boost::asio::posix::stream_descriptor::wait_write;

    EventId id = m_id;
    short persist = (m_listen_events & EV_PERSIST);
    int fd = m_fd;

    m_sd->async_wait(wait_type,
        [this, id, event_flag, persist, fd](boost::system::error_code ec) {
            if (ec == boost::asio::error::operation_aborted)
                return;

            // Plan C: fd 就绪时取消 timer，防止 double-fire
            if (m_timer) {
                m_timer->cancel();
            }

            CallEventCallback(id, fd, event_flag);

            // PERSIST: 重新注册
            if (persist && m_sd) {
                DoAsyncWait(event_flag);
            }
        });
}

int Event::CancelListen(bool need_close_fd)
{
    DelEventCallback(m_id);

    // 取消所有挂起的异步操作
    if (m_sd) {
        boost::system::error_code ec;
        m_sd->cancel(ec);
        // flush 掉所有被取消的 handler
        m_ref_base->GetContext().restart();
        while (m_ref_base->GetContext().poll() > 0) {}
        if (need_close_fd && m_fd >= 0) {
            m_sd->release();
            ::close(m_fd);
        } else {
            // 不关 fd — 先 release 防止 stream_descriptor 析构关掉
            m_sd->release();
        }
        m_sd.reset();
    }

    if (m_timer) {
        m_timer->cancel();
        m_timer.reset();
    }

    if (need_close_fd && m_fd >= 0 && !m_sd) {
        ::close(m_fd);
    }

    m_timeout = -1;
    return 0;
}

int Event::Trigger(int flag)
{
    // Plan C: Trigger 时取消所有挂起的异步操作，防止与其他路径 double-fire
    if (m_timer) {
        m_timer->cancel();
    }
    if (m_sd) {
        boost::system::error_code ignored;
        m_sd->cancel(ignored);
    }

    EventId id = m_id;
    int fd = m_fd;
    boost::asio::post(m_ref_base->GetContext(), [id, fd, flag]() {
        CallEventCallback(id, fd, static_cast<short>(flag));
    });
    return 0;
}

int Event::GetSocket() const
{
    return m_fd;
}

short Event::GetEvents() const
{
    return m_listen_events;
}

EventId Event::GetEventId()
{
    return m_id;
}

int64_t Event::GetTimeoutMs() const
{
    return m_timeout;
}

EventId Event::GenerateId()
{
    static std::atomic_uint64_t _id{0};
    return (++_id);
}

void Event::AddEventCallback(EventId id, const OnEventCallback& cb)
{
    std::lock_guard<std::mutex> lock(m_callback_map_mtx);
    m_callback_map[id] = cb;
}

void Event::DelEventCallback(EventId id)
{
    std::lock_guard<std::mutex> lock(m_callback_map_mtx);
    m_callback_map.erase(id);
}

void Event::CallEventCallback(EventId id, int fd, short events)
{
    OnEventCallback cb = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_callback_map_mtx);
        auto it = m_callback_map.find(id);
        if (it != m_callback_map.end())
            cb = it->second;
    }
    if (cb) cb(fd, events, id);
}

} // namespace bbt::pollevent
