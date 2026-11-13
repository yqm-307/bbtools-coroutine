/**
 * @file bbt/pollevent/EventLoop.hpp
 * @brief 事件循环的创建、驱动与停止接口。
 */
#pragma once
#include <bbt/pollevent/detail/EventBase.hpp>
#include <bbt/pollevent/detail/Define.hpp>
#include <memory>

namespace bbt::pollevent {

class Event;

/**
 * @brief 驱动文件描述符、定时器和主动触发事件的事件循环。
 *
 * CreateEvent 创建事件，Event::StartListen 登记等待；调用 StartLoop 才会驱动
 * 底层 EventBase 派发事件，回调在调用 StartLoop 的线程上执行。本类不创建后台线程。
 *
 * 默认构造时创建并持有一个 EventBase；使用外部 EventBase 时，由构造参数决定
 * 是否接管释放责任。EventBase 必须活过关联的全部 Event，不能先释放底层对象
 * 再销毁事件。
 *
 * 销毁前须保证没有线程仍在 StartLoop 中。BreakLoop 只请求停止，不等待驱动线程
 * 退出；由调用方负责等待线程退出，并在释放 EventBase 前销毁关联事件。
 */
class EventLoop {
    friend class Event;

public:
    typedef std::shared_ptr<EventLoop> SPtr;

    /**
     * @brief 使用已有的 EventBase 构造事件循环，不启动事件派发。
     *
     * @param base 非空的底层事件对象，必须活过本循环的使用期及其关联的全部 Event。
     * @param need_auto_free_base 为 true 时由本对象在析构时 delete base，调用方不得
     *        再次释放；为 false 时仅借用，释放责任仍归调用方。默认接管释放责任。
     */
    explicit EventLoop(detail::EventBase* base, bool need_auto_free_base = true);

    EventLoop();

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

    /**
     * @brief 存活 Event 对象数（构造 +1、析构 -1），不是待派发事件数；未监听或已交付未析构的 Event 都算在内。
     */
    int GetEventNum();

    /**
     * @brief 构造一个 Event，不登记等待；等待要到 Event::StartListen 才挂到底层 context。
     * @return Event 的 shared_ptr；调用方必须持续持有，否则 StartListen/Trigger 因无 shared ownership 返回 -1。
     */
    std::shared_ptr<Event> CreateEvent(evutil_socket_t fd, short events, const OnEventCallback& onevent_cb);

    /**
     * @brief 单调时钟毫秒数（steady_clock），不受系统时间调整影响。
     */
    int64_t GetEvMonotonic() const;

    /**
     * @brief 取当前墙钟写入 @p tv（不可为空）；名字沿用 libevent 缓存接口，实际每次直读 system_clock，恒返回 0。
     */
    int GetTimeOfDayCached(struct timeval* tv) const;

    /**
     * @brief 墙钟毫秒数（tv_sec*1000 + tv_usec/1000）；GetTimeOfDayCached 非 0 时原样返回该值。
     */
    int64_t GetTime() const;

    /**
     * 内部
     */

    /**
     * @brief 取底层 EventBase；拿到 context 的调用方不得另行 run/poll/stop/restart。
     */
    detail::EventBase* GetEventBase() { return m_ev_base; }

private:
    detail::EventBase* m_ev_base{nullptr};
    bool m_auto_free_base{false};
};

}
