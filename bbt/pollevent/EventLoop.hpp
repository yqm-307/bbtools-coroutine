/**
 * @file bbt/pollevent/EventLoop.hpp
 * @brief 事件循环门面：持有一个 detail::EventBase 并驱动其派发；回调在调用 StartLoop 的线程上执行。
 */
#pragma once
#include <bbt/pollevent/detail/EventBase.hpp>
#include <bbt/pollevent/detail/Define.hpp>
#include <memory>

namespace bbt::pollevent {

class Event;

/** @brief 事件循环门面：驱动 detail::EventBase 的 io_context，派发 FD / Timer / Trigger 三类事件。 */
class EventLoop {
    friend class Event;

public:
    typedef std::shared_ptr<EventLoop> SPtr;

    /** @brief 接管外部 EventBase；@p need_auto_free_base 决定析构时是否 delete base，base 须活过本对象。 */
    explicit EventLoop(detail::EventBase* base, bool need_auto_free_base = true);

    /** @brief 自建一个 detail::EventBase，析构时一并释放。 */
    EventLoop();

    /** @brief 按 m_auto_free_base 释放底层 EventBase；调用方须先保证没有线程仍在 StartLoop 内。 */
    ~EventLoop();

    /**
     * @brief 按 opt 驱动一次派发；未知 opt 等同 LOOP_NORMAL，调用者即驱动线程。
     * @return 0 = 派发过回调；1 = 无事件可派发或 context 已 stopped；只有 NONBLOCK/NO_EXIT_ON_EMPTY 会先 restart。
     */
    int StartLoop(int opt);

    /**
     * @brief 请求停止驱动：置 io_context 停止标记，使正在 StartLoop 的线程尽快返回。
     * @return 恒为 0；可从任意线程调用，已进入执行的回调不会被打断，停止后也不自动 restart。
     */
    int BreakLoop();

    /** @brief 存活 Event 对象数（构造 +1、析构 -1），不是待派发事件数；未监听或已交付未析构的 Event 都算在内。 */
    int GetEventNum();

    /**
     * @brief 构造一个 Event，不登记等待；等待要到 Event::StartListen 才挂到底层 context。
     * @return Event 的 shared_ptr；调用方必须持续持有，否则 StartListen/Trigger 因无 shared ownership 返回 -1。
     */
    std::shared_ptr<Event> CreateEvent(evutil_socket_t fd, short events, const OnEventCallback& onevent_cb);

    /** @brief 单调时钟毫秒数（steady_clock），不受系统时间调整影响。 */
    int64_t GetEvMonotonic() const;

    /** @brief 取当前墙钟写入 @p tv（不可为空）；名字沿用 libevent 缓存接口，实际每次直读 system_clock，恒返回 0。 */
    int GetTimeOfDayCached(struct timeval* tv) const;

    /** @brief 墙钟毫秒数（tv_sec*1000 + tv_usec/1000）；GetTimeOfDayCached 非 0 时原样返回该值。 */
    int64_t GetTime() const;

    /* 内部 */
    /** @brief 取底层 EventBase；拿到 context 的调用方不得另行 run/poll/stop/restart。 */
    detail::EventBase* GetEventBase()
    {
        return m_ev_base;
    }

private:
    detail::EventBase* m_ev_base{nullptr};
    bool m_auto_free_base{false};
};

} // namespace bbt::pollevent
