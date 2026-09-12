#include <atomic>
#include <thread>

#include <bbt/core/util/Assert.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>

namespace bbt::coroutine::detail
{

/* StartLoop 内的在 poll 标记；handler 抛异常也不遗留 m_in_poll == true。 */
class InPollGuard
{
public:
    explicit InPollGuard(std::atomic<bool>& flag): m_flag(flag) { m_flag.store(true, std::memory_order_release); }
    ~InPollGuard() { m_flag.store(false, std::memory_order_release); }
private:
    std::atomic<bool>& m_flag;
};

CoPoller::UPtr& CoPoller::GetInstance()
{
    static UPtr _inst = nullptr;
    if (_inst == nullptr)
        _inst = UPtr{new CoPoller()};
    
    return _inst;
}


CoPoller::CoPoller()
{
    /* 首个 backend：bbt::pollevent::EventLoop（ASIO）。不是 epoll/timerfd。 */
    auto* base = new bbt::pollevent::detail::EventBase(
        bbt::pollevent::detail::EventBaseConfigFlag::NO_CACHE_TIME |
        bbt::pollevent::detail::EventBaseConfigFlag::PRECISE_TIMER);
    
    Assert(base != nullptr);

    m_event_loop = std::make_shared<bbt::pollevent::EventLoop>(base, true);
    Assert(m_event_loop != nullptr);
}

CoPoller::~CoPoller()
{
}


std::shared_ptr<bbt::pollevent::Event> CoPoller::CreateEvent(int fd, short events, const bbt::pollevent::OnEventCallback& onevent_cb)
{
    return m_event_loop->CreateEvent(fd, events, onevent_cb);
}

bool CoPoller::PollOnce()
{
    errno = 0;

    /* 谁首次驱动谁拥有析构权；已写入不覆盖（驱动线程唯一，Scheduler 单入口）。 */
    std::thread::id no_driver{};
    m_driver_tid.compare_exchange_strong(no_driver, std::this_thread::get_id(),
                                         std::memory_order_acq_rel);

    /* 兑现顺序：先兑现积压 Event，再 StartLoop 驱动。
       积压必须先于下一拍注册释放，否则同一 FD 会在 ASIO 重复注册。 */
    {
        std::vector<std::shared_ptr<bbt::pollevent::Event>> deferred;
        {
            std::lock_guard<std::mutex> lock(m_deferred_mutex);
            deferred.swap(m_deferred_destroy);
        }
        /* 锁外析构，避免 Event::CancelListen 回调路径反向获取本锁。 */
    }

    InPollGuard in_poll_guard{m_in_poll};
    return (m_event_loop->StartLoop(bbt::pollevent::EventLoopOpt::LOOP_NONBLOCK) == 0);
}

int CoPoller::NotifyCustomEvent(std::shared_ptr<CoPollEvent> event)
{
    Assert(event != nullptr);
    return event->Trigger(POLL_EVENT_CUSTOM);
}

void CoPoller::DeferDestroyEvent(std::shared_ptr<bbt::pollevent::Event> event)
{
    if (event == nullptr) return;
    std::lock_guard<std::mutex> lock(m_deferred_mutex);
    m_deferred_destroy.push_back(std::move(event));
}

void CoPoller::FlushDeferredEvents()
{
    /* 只在驱动线程且不在 poll 内兑现；其它线程调用是 no-op（含尚未有驱动
       线程时），积压延迟到驱动线程下一次 PollOnce 兑现（见 PollOnce 注释）。 */
    if (m_driver_tid.load(std::memory_order_acquire) != std::this_thread::get_id())
    {
        m_drain_off_driver_cnt.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (m_in_poll.load(std::memory_order_acquire))
    {
        m_drain_in_poll_cnt.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    std::vector<std::shared_ptr<bbt::pollevent::Event>> deferred;
    {
        std::lock_guard<std::mutex> lock(m_deferred_mutex);
        deferred.swap(m_deferred_destroy);
    }
    /* 在锁外析构，避免 Event::CancelListen 回调路径反向获取本锁。 */
}

uint64_t CoPoller::GetDrainOffDriverCount() const
{
    return m_drain_off_driver_cnt.load(std::memory_order_relaxed);
}

uint64_t CoPoller::GetDrainInPollCount() const
{
    return m_drain_in_poll_cnt.load(std::memory_order_relaxed);
}

std::shared_ptr<bbt::pollevent::EventLoop> CoPoller::GetEventLoop() const
{
    return m_event_loop;
}

}