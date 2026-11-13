/**
 * @file bbt/pollevent/detail/EventBase.hpp
 * @brief Event 与 EventLoop 共用的底层 context：一个 Asio io_context 加存活事件计数；Event 只借用裸指针，不得活过它。
 */
#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <chrono>

namespace bbt::pollevent::detail {

/**
 * @brief 兼容保留的配置位（libevent 时代的 flag）；Asio io_context 无等价开关，所有取值都被忽略。
 */
enum EventBaseConfigFlag : int32_t {
    NOLOCK = 0,
    IGNORE_ENV = 1,
    STARTUP_IOCP = 2,
    NO_CACHE_TIME = 4,
    EPOLL_USE_CHANGELIST = 8,
    PRECISE_TIMER = 16,
};

/**
 * @brief 事件层底层 context：持有唯一 io_context，并记录存活 Event 对象数。
 */
class EventBase {
public:
    /**
     * @brief 构造空的 io_context；@p flag 兼容保留，忽略。
     */
    explicit EventBase(int32_t flag = 0);

    /**
     * @brief 析构 io_context；调用方须保证没有线程仍在它上面派发。
     */
    ~EventBase();

    /**
     * @brief 存活 Event 对象数（构造 +1、析构 -1），不是待派发事件数。
     */
    int GetEventNum() const;

    /**
     * @brief 读当前墙钟写入 @p tv（不可为空），恒返回 0；名字沿用 libevent 缓存接口，实际每次直读 system_clock。
     */
    int GetTimeOfDayCache(struct timeval* tv) const;

    /**
     * 内部接口
     */

    /**
     * @brief 取唯一 io_context；只允许 EventLoop 的驱动线程在其上 run/poll，其它线程不得另行驱动。
     */
    boost::asio::io_context& GetContext() { return m_io_ctx; }

    /**
     * 由 Event 构造调用；计数是存活对象数，与是否已派发无关。
     */
    void IncEventCount() { m_event_count++; }

    /**
     * 由 Event 析构调用，与 IncEventCount 成对。
     */
    void DecEventCount() { m_event_count--; }

private:
    boost::asio::io_context m_io_ctx;
    std::atomic_int m_event_count{0};
};

}
