#include <bbt/pollevent/detail/EventBase.hpp>

namespace bbt::pollevent::detail
{

EventBase::EventBase(int32_t flag)
{
    // ASIO io_context 无等价配置标志，flag 保留兼容，实际忽略
    (void)flag;
}

EventBase::~EventBase()
{
    // io_context 自动析构
}

int EventBase::GetEventNum() const
{
    return m_event_count.load();
}

int EventBase::GetTimeOfDayCache(struct timeval* tv) const
{
    auto now = std::chrono::system_clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch());
    auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch() - secs);

    tv->tv_sec  = secs.count();
    tv->tv_usec = usecs.count();
    return 0;
}

} // namespace bbt::pollevent::detail
