#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
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
 *
 * Event 析构所有权：底层 Event 只能在驱动线程（第一个调用 PollOnce 的线程）
 * 上兑现。FlushDeferredEvents 在非驱动线程调用是 no-op，积压延迟到驱动线程
 * 下一次 PollOnce（进入 poll 前）兑现。
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

    /* 延迟销毁 Event；由 FlushDeferredEvents 在唤醒协程前兑现。 */
    void                            DeferDestroyEvent(std::shared_ptr<bbt::pollevent::Event> event);

    /* 事件完成后，在唤醒协程重新注册同一 FD 前释放底层 Event。
       只在驱动线程且不在 poll 内兑现；其它线程调用是 no-op，延迟到下一次 PollOnce。 */
    void                            FlushDeferredEvents();

    /* 只读诊断计数（回归测试断言用）：no-op 路径的调用次数。
       驱动线程正常兑现不加计数。 */
    uint64_t                        GetDrainOffDriverCount() const;
    uint64_t                        GetDrainInPollCount() const;

    std::shared_ptr<bbt::pollevent::EventLoop>
                                    GetEventLoop() const;
private:
    std::shared_ptr<bbt::pollevent::EventLoop> m_event_loop{nullptr};
    std::vector<std::shared_ptr<bbt::pollevent::Event>> m_deferred_destroy;
    std::mutex                      m_deferred_mutex;

    /* 首个调用 PollOnce 的线程（惰性写入，谁驱动谁拥有析构权） */
    std::atomic<std::thread::id>    m_driver_tid{};
    /* 驱动线程是否在 StartLoop 内；handler 内嵌套 Flush 一律 no-op */
    std::atomic<bool>               m_in_poll{false};
    std::atomic<uint64_t>           m_drain_off_driver_cnt{0};
    std::atomic<uint64_t>           m_drain_in_poll_cnt{0};
};

}
