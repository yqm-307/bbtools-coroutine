#pragma once
#include <memory>
#include <mutex>
#include <vector>
#include <bbt/pollevent/EventLoop.hpp>
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail
{

class CoPollEvent;

/**
 * EventLoop 门面。Scheduler 只通过 PollOnce() 驱动，不依赖 epoll。
 *
 * 首版三类事件：
 *   FD      — CreateEvent(fd, READABLE/WRITEABLE)
 *   Timer   — CreateEvent(-1, TIMEOUT)
 *   Wakeup  — NotifyCustomEvent
 *
 * Poller backend 是 bbt::pollevent::EventLoop（当前 ASIO），可替换。
 * epoll 不是用户契约。IPoller 是未接入遗留接口，不在本阶段实现。
 */
class CoPoller
{
public:
    typedef std::unique_ptr<CoPoller> UPtr;
    static UPtr&                    GetInstance();

    CoPoller();
    ~CoPoller();

    bool                            PollOnce();

    std::shared_ptr<bbt::pollevent::Event>
                                    CreateEvent(int fd, short events, const bbt::pollevent::OnEventCallback& onevent_cb);

    /* 线程安全，多次调用仅第一次有效 */
    int                             NotifyCustomEvent(std::shared_ptr<CoPollEvent> event);

    /* 延迟销毁 Event —— 在 Scheduler/PollOnce 线程安全执行 */
    void                            DeferDestroyEvent(std::shared_ptr<bbt::pollevent::Event> event);

    std::shared_ptr<bbt::pollevent::EventLoop>
                                    GetEventLoop() const;
private:
    std::shared_ptr<bbt::pollevent::EventLoop> m_event_loop{nullptr};
    std::vector<std::shared_ptr<bbt::pollevent::Event>> m_deferred_destroy;
    std::mutex                      m_deferred_mutex;
};

}
