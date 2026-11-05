#include <bbt/pollevent/EventLoop.hpp>
#include <bbt/pollevent/Event.hpp>
#include <bbt/pollevent/detail/EventBase.hpp>
#include <boost/asio.hpp>
#include <chrono>

namespace bbt::pollevent {

EventLoop::EventLoop(detail::EventBase* base, bool need_auto_free_base)
    : m_ev_base(base), m_auto_free_base(need_auto_free_base)
{
    // 外部 base 的释放责任由 need_auto_free_base 决定；空指针不在此校验，后果在首次 StartLoop/CreateEvent 暴露。
}

EventLoop::EventLoop() : m_ev_base(new detail::EventBase), m_auto_free_base(true)
{
}

EventLoop::~EventLoop()
{
    // 驱动线程必须先退出：base 被 delete 后，其上挂起的等待与执行中的回调都失去所属 context
    if (m_auto_free_base) {
        delete m_ev_base;
        m_ev_base = nullptr;
    }
}

int EventLoop::StartLoop(int opt)
{
    // 单次派发：调用者即驱动线程，完成回调在本线程内跑；返回 0 = 派发过事件，1 = 没有。
    auto& ctx = m_ev_base->GetContext();
    int dispatched = 0;

    switch (opt) {
    case EventLoopOpt::LOOP_NONBLOCK: {
        // 非阻塞：仅处置已就绪事件，无就绪立即返回；restart 抵消上一轮可能已置的 stop。
        ctx.restart();
        dispatched = static_cast<int>(ctx.poll());
        return (dispatched > 0) ? 0 : 1;
    }

    case EventLoopOpt::LOOP_ONCE:
        // 阻塞：等待一个事件就绪并处置后返回；与 default 分支实现相同，都不 restart。
        dispatched = static_cast<int>(ctx.run_one());
        return (dispatched > 0) ? 0 : 1;

    case EventLoopOpt::LOOP_NO_EXIT_ON_EMPTY:
        // 阻塞：无就绪事件时不退出，继续等待；同样靠 restart 抵消上一轮的 stop。
        ctx.restart();
        dispatched = static_cast<int>(ctx.run_one());
        return (dispatched > 0) ? 0 : 1;

    default: {
        // LOOP_NORMAL（以及任何未知 opt）：阻塞，处置一个就绪事件后返回
        dispatched = static_cast<int>(ctx.run_one());
        return (dispatched > 0) ? 0 : 1;
    }
    }
}

int EventLoop::BreakLoop()
{
    // 只置停止标记，不 join 也不销毁对象，可从非驱动线程调用；已进入执行的回调照常跑完。
    m_ev_base->GetContext().stop();
    return 0;
}

std::shared_ptr<Event> EventLoop::CreateEvent(evutil_socket_t fd, short events, const OnEventCallback& onevent_cb)
{
    // 构造只建 asio 对象与回调登记项，不挂等待；真正登记在 Event::StartListen。
    auto event_sptr = std::make_shared<Event>(m_ev_base, fd, events, onevent_cb);
    return event_sptr;
}

int EventLoop::GetEventNum()
{
    return m_ev_base->GetEventNum();
}

int64_t EventLoop::GetEvMonotonic() const
{
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return static_cast<int64_t>(ms);
}

int EventLoop::GetTimeOfDayCached(struct timeval* tv) const
{
    return m_ev_base->GetTimeOfDayCache(tv);
}

int64_t EventLoop::GetTime() const
{
    // 折算成毫秒；tv_usec 只取毫秒位，调用方不要期待微秒精度。
    struct timeval tv;
    int ret = GetTimeOfDayCached(&tv);
    if (ret != 0)
        return ret;
    return (static_cast<int64_t>(tv.tv_sec) * 1000 + static_cast<int64_t>(tv.tv_usec) / 1000);
}

} // namespace bbt::pollevent
