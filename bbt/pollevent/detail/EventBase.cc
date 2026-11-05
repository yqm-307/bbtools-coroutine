#include <bbt/pollevent/detail/EventBase.hpp>

namespace bbt::pollevent::detail {

EventBase::EventBase(int32_t flag)
{
    // ASIO io_context 无等价配置标志，flag 保留兼容，实际忽略
    (void)flag;
}

EventBase::~EventBase()
{
    // io_context 自动析构；调用方须保证驱动线程已退出
}

int EventBase::GetEventNum() const
{
    // 存活对象数，不是待派发数：未监听或已交付未析构的 Event 都算在内
    return m_event_count.load();
}

int EventBase::GetTimeOfDayCache(struct timeval* tv) const
{
    // 名字沿用 libevent 的缓存接口，实际每次都读 system_clock；墙钟回拨会直接反映到结果里
    auto now = std::chrono::system_clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
    auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch() - secs);

    tv->tv_sec = secs.count();
    tv->tv_usec = usecs.count();
    return 0;
}

} // namespace bbt::pollevent::detail
