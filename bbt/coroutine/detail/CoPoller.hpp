#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <boost/asio/any_io_executor.hpp>
#include <bbt/pollevent/EventLoop.hpp>
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail
{

class CoPollEvent;

/**
 * EventLoop 门面。Scheduler 只通过 PollOnce()（非阻塞排空）/PollOnceUntil()（有界
 * 事件等待）驱动，不依赖 epoll。
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

    /**
     * @brief 有界事件等待：停拍期间不空等，也不无限阻塞。
     *
     * 驱动语义与 PollOnce 相同（driver-tid 所有权、进入驱动前兑现延迟析构、
     * m_in_poll 保护、锁外析构 Event），区别只在「没有就绪事件时怎么办」：
     *   - EventLoop 已注册事件数（对象数，不是 pending 数）> 0：阻塞在内部
     *     EventLoop 上，直到任一 fd/timer/custom 事件被派发，或到达 deadline
     *     （先到先醒）；不新增线程，也不改变 EventLoop 既有模式语义；
     *   - 事件数 == 0：与旧路径一致按 deadline 定时休眠，不进入 Asio 等待
     *     （无超时的 custom Chan 等纯协程等待不因此多付一次事件循环开销）；
     *   - deadline 已过期：立即返回 false，不驱动 context（非阻塞）。
     *
     * @return 本次等待期间是否派发了事件；到点返回 false。
     *
     * 已知边界：等待开始后才首次登记的新 FD 依赖底层 reactor 是否唤醒当前
     * 等待，不保证立即打断，最迟在 deadline 处收敛——调度器传入的 deadline
     * 即下一次扫描拍。deadline 用 steady_clock 绝对时刻，不接受按事件次数
     * 推算的等待量：密集事件不得推迟维护节拍。私有 detail 接口，不进入
     * 公开契约。
     */
    bool                            PollOnceUntil(std::chrono::steady_clock::time_point deadline);

    std::shared_ptr<bbt::pollevent::Event>
                                    CreateEvent(int fd, short events, const bbt::pollevent::OnEventCallback& onevent_cb);

    /* 线程安全，多次调用仅第一次有效 */
    int                             NotifyCustomEvent(std::shared_ptr<CoPollEvent> event);

    /* 取消语义的唤醒入口：以独立取消位触发，供等待者向取消令牌
     * 登记的唤醒回调使用；与 NotifyCustomEvent（完成语义）走不同位，
     * 恢复侧可凭唤醒掩码直接区分。线程安全，多次调用仅第一次有效。 */
    int                             NotifyCancelEvent(std::shared_ptr<CoPollEvent> event);

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

    /* 只读投递入口：返回本 Poller 所持 EventLoop 的 Asio executor。
       executor 绑定的 io_context 随本实例生命周期存在；不暴露原始
       io_context，调用方无法经此句柄 stop/restart/run。 */
    boost::asio::any_io_executor    GetExecutor() const;
private:
    /* PollOnce/PollOnceUntil 共用的驱动前置：登记 driver-tid（首个驱动者拥有
       Event 析构权），并在驱动前兑现积压 Event——同一 FD 的下一拍注册要求旧
       底层 Event 先释放，否则 ASIO 重复注册。 */
    void                            _BeginDrive();

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
