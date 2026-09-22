#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <chrono>

namespace bbt::pollevent::detail
{

/* 保留配置枚举以兼容旧 API（ASIO 无等价物，忽略） */
enum EventBaseConfigFlag: int32_t
{
    NOLOCK                  = 0,
    IGNORE_ENV              = 1,
    STARTUP_IOCP            = 2,
    NO_CACHE_TIME           = 4,
    EPOLL_USE_CHANGELIST    = 8,
    PRECISE_TIMER           = 16,
};

class EventBase
{
public:
    explicit EventBase(int32_t flag = 0);
    ~EventBase();

    int                     GetEventNum() const;
    int                     GetTimeOfDayCache(struct timeval* tv) const;

    /* 内部接口 */
    boost::asio::io_context& GetContext() { return m_io_ctx; }
    void                     IncEventCount()   { m_event_count++; }
    void                     DecEventCount()   { m_event_count--; }

private:
    boost::asio::io_context m_io_ctx;
    std::atomic_int         m_event_count{0};
};

} // namespace bbt::pollevent::detail
