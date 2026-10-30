#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <boost/asio/post.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/pollevent/Event.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/Defer.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>


using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(CoPollerTest)

BOOST_AUTO_TEST_CASE(t_poller_timeout_event_single)
{
    std::atomic_int count = 0;
    const int stack_size = 4096;
    const int timeout_ms = 1000;

    auto co_sptr = Coroutine::Create(stack_size, [](){});
    auto event = CoPollEvent::Create(co_sptr->GetId(),[&](auto event, int, int){
        count++;
    });

    BOOST_ASSERT(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, timeout_ms) == 0);
    BOOST_ASSERT(event->Regist() == 0);
    BOOST_REQUIRE(!event->CommitPark());

    while (count != 1)
    {
        CoPoller::GetInstance()->PollOnce();
        std::this_thread::sleep_for(bbt::core::clock::milliseconds(1));
    }
    BOOST_CHECK(count == 1);
}

BOOST_AUTO_TEST_CASE(t_poller_evnet_cancel)
{
    std::atomic_bool flag = false;
    const int stack_size = 4096;
    const int timeout_ms = 200;

    auto co_sptr = Coroutine::Create(stack_size, [](){});
    auto event = CoPollEvent::Create(co_sptr->GetId(), [&](auto event, int, int){
        flag.exchange(true);
    });

    Assert(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, timeout_ms) == 0);
    Assert(event->Regist() == 0);

    event->UnRegist();

    sleep(1);
    BOOST_CHECK_EQUAL(flag, false);
}

BOOST_AUTO_TEST_CASE(t_poller_timerout_event_multi)
{
    std::atomic_int count = 0;
    std::vector<CoPollEvent::SPtr> events;

    /* 创建并注册1000个定时事件 */
    for (int i = 0; i < 1000; ++i)
    {
        auto co_sptr = Coroutine::Create(4096, [](){});
        auto event = CoPollEvent::Create(co_sptr->GetId(), [&count, co_sptr](auto, int, int){
            count++;
        });

        /* 初始化并注册事件 */
        BOOST_ASSERT(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, 500) == 0);
        BOOST_ASSERT(event->Regist() == 0);
        BOOST_REQUIRE(!event->CommitPark());
        events.push_back(event);
    }

    // 非阻塞情况下程序最多活10s
    auto max_end_ts = bbt::core::clock::nowAfter(bbt::core::clock::seconds(2));

    /* 开始轮询，探测完成的事件并回调通知到协程事件完成 */
    while (!bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(max_end_ts))
    {
        CoPoller::GetInstance()->PollOnce();
        std::this_thread::sleep_for(bbt::core::clock::milliseconds(2));
    }

    /* 所有超时时间完成，开始判断是否达成预期 */
    BOOST_CHECK_EQUAL(count.load(), 1000);
}

BOOST_AUTO_TEST_CASE(t_poller_timerout_event_multi_thread)
{
    /* 多线程注册事件，单线程处理（reactor模型） */

    std::atomic_int                 count = 0;
    std::vector<std::thread*>       threads;
    const int                       m_timeout_ms = 500;

    std::vector<CoPollEvent::SPtr>  events;
    std::mutex                      events_mutex;

    for (int i = 0; i < 2; ++i)
    {
        auto t = new std::thread([&](){
            for (int i = 0; i < 5000; ++i)
            {
                auto co_sptr = Coroutine::Create(4096, [](){}, false);
                auto event = CoPollEvent::Create(co_sptr->GetId(), [&count, co_sptr](auto, int, int){
                    count++;
                });

                BOOST_ASSERT(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, m_timeout_ms) == 0);
                BOOST_ASSERT(event->Regist() == 0);
                BOOST_REQUIRE(!event->CommitPark());

                events_mutex.lock();
                events.push_back(event);
                events_mutex.unlock();
            }
        });
        threads.push_back(t);
    }

    for (auto&& item : threads)
    {
        if (item->joinable())
            item->join();
    }

    // 非阻塞情况下程序最多活10s
    auto max_end_ts = bbt::core::clock::nowAfter(bbt::core::clock::seconds(2));

    /* 所有注册线程完成后，单线程轮询并回调通知到协程事件完成 */
    while (!bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(max_end_ts))
    {
        CoPoller::GetInstance()->PollOnce();
        std::this_thread::sleep_for(bbt::core::clock::microseconds(2));
    }

    BOOST_CHECK_EQUAL(count, 10000);
}

BOOST_AUTO_TEST_SUITE_END()

/* ── PollOnceUntil：停拍有界事件等待的回归 ──────────────────────────
 *
 * 每个用例用本文件内独立构造的 CoPoller 实例：driver-tid、in_poll、延迟析构
 * 队列和已注册事件数都不与上面的全局单例用例共享，避免用例顺序耦合。
 *
 * 断言口径（不做微秒级墙钟断言）：
 *   - 精确事实用同步条件判定：回调次数、返回值、计数器；
 *   - 时间只做宽松上下界，用来区分三种错误形态：
 *       a) 忽略就绪事件、一直等到 deadline  → 派发用例的 elapsed 上界；
 *       b) 空 context 立即返回（忙轮询退化） → 空等用例的 elapsed 下界；
 *       c) 回归成不可打断的固定睡眠          → 派发用例的 elapsed 上界。
 * 宿主机抖动只会让这些宽松上界更晚命中，不造成假失败；上界同时是 fail-safe，
 * 保证用例自身不会无界等待。
 */
BOOST_AUTO_TEST_SUITE(CoPollerBoundedWaitTest)

namespace
{
/* 只用于判「没有挂死」的宽松上界，不是性能论断。 */
const auto kBoundedWaitCeiling = std::chrono::seconds(5);

/* 已就绪 fd 的一次性读事件；返回事件对象，调用方负责其寿命。 */
std::shared_ptr<bbt::pollevent::Event> MakeReadableEvent(CoPoller& poller, int fd, std::atomic<int>& fired)
{
    auto event = poller.CreateEvent(fd, bbt::pollevent::EventOpt::READABLE,
        [&fired](int listen_fd, short, bbt::pollevent::EventId) {
            char byte = '\0';
            if (::read(listen_fd, &byte, 1) == 1 && byte == 'x')
                fired.fetch_add(1);
        });
    if (event != nullptr)
        event->StartListen(0);
    return event;
}

std::shared_ptr<bbt::pollevent::Event> MakeTimerEvent(CoPoller& poller, std::atomic<int>& fired, uint64_t timeout_ms)
{
    auto timer = poller.CreateEvent(-1, bbt::pollevent::EventOpt::TIMEOUT,
        [&fired](int, short, bbt::pollevent::EventId) { fired.fetch_add(1); });
    if (timer != nullptr)
        timer->StartListen(timeout_ms);
    return timer;
}
}

/* 进入等待前就已就绪的 fd：必须当场派发，不能等满 deadline。 */
BOOST_AUTO_TEST_CASE(t_wait_dispatches_ready_fd_before_deadline)
{
    CoPoller poller;

    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);
    const char byte = 'x';
    BOOST_REQUIRE_EQUAL(::write(fds[1], &byte, 1), 1);

    std::atomic<int> fired{0};
    auto event = MakeReadableEvent(poller, fds[0], fired);
    BOOST_REQUIRE(event != nullptr);

    const auto begin = std::chrono::steady_clock::now();
    const bool dispatched = poller.PollOnceUntil(begin + std::chrono::seconds(2));
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    BOOST_CHECK(dispatched);
    BOOST_CHECK_EQUAL(fired.load(), 1);
    /* 错误形态 a：会耗满 2s。 */
    BOOST_CHECK(elapsed < std::chrono::seconds(1));

    ::close(fds[0]);
    ::close(fds[1]);
}

/* 等待期间 fd 变就绪（注册在前、灌数据在后）：等待必须被打断而不是睡到 deadline。 */
BOOST_AUTO_TEST_CASE(t_wait_wakes_on_fd_ready)
{
    CoPoller poller;

    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);

    std::atomic<int> fired{0};
    std::atomic<bool> writer_go{false};
    std::atomic<int> write_rlt{0};
    auto event = MakeReadableEvent(poller, fds[0], fired);
    BOOST_REQUIRE(event != nullptr);

    std::thread writer([&] {
        while (!writer_go.load(std::memory_order_acquire))
            std::this_thread::yield();
        const char byte = 'x';
        write_rlt.store(static_cast<int>(::write(fds[1], &byte, 1)));
    });

    writer_go.store(true, std::memory_order_release);
    const auto begin = std::chrono::steady_clock::now();
    const bool dispatched = poller.PollOnceUntil(begin + std::chrono::seconds(2));
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    writer.join();

    BOOST_CHECK_EQUAL(write_rlt.load(), 1);
    BOOST_CHECK(dispatched);
    BOOST_CHECK_EQUAL(fired.load(), 1);
    BOOST_CHECK(elapsed < std::chrono::seconds(1));

    ::close(fds[0]);
    ::close(fds[1]);
}

/* 等待期间 timer 到期：定时事件必须打断等待并派发。 */
BOOST_AUTO_TEST_CASE(t_wait_wakes_on_timer)
{
    CoPoller poller;
    std::atomic<int> fired{0};

    /* 允许注册完成后被宿主调度暂停；派发计数确认 timer 到期，上界排除等满 deadline。 */
    auto timer = MakeTimerEvent(poller, fired, 30);
    BOOST_REQUIRE(timer != nullptr);

    const auto begin = std::chrono::steady_clock::now();
    const bool dispatched = poller.PollOnceUntil(begin + std::chrono::seconds(2));
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    BOOST_CHECK(dispatched);
    BOOST_CHECK_EQUAL(fired.load(), 1);
    /* 注册后若已过 30ms，允许立即派发；不能用相对等待下界代替到期事实。 */
    BOOST_CHECK(elapsed < std::chrono::seconds(1));
}

/* 等待期间的跨线程 post：必须打断等待，且 handler 仍在驱动线程执行。
 *
 * post 只保证「不丢、且排在已派发队列里」，它不像 fd/timer 那样计入
 * GetEventNum()。所以这里先挂一个远期 timer 让本实例走事件等待路径——这也正是
 * 生产形态：custom 唤醒（NotifyCustomEvent/NotifyCancelEvent）总伴随已注册的
 * CoPollEvent。没有已注册事件的纯 post 只会在本次定时休眠后由下一次 poll 兑现。 */
BOOST_AUTO_TEST_CASE(t_wait_wakes_on_cross_thread_post)
{
    CoPoller poller;
    const auto driver_tid = std::this_thread::get_id();
    const int rounds = 8;

    /* 远期锚点：只为让「已注册事件数 > 0」，本身不应在本用例内到期。 */
    std::atomic<int> anchor_fired{0};
    auto anchor = MakeTimerEvent(poller, anchor_fired, 5000);
    BOOST_REQUIRE(anchor != nullptr);

    std::atomic<int> gate{0};
    std::atomic<int> handled{0};
    std::atomic<int> handled_on_driver{0};

    /* 各轮共用同一 deadline：等待在提前唤醒后保留同一条截止等待，不重新 arm，
       于是每轮返回都只可能来自 post 派发。 */
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    std::thread poster([&] {
        for (int r = 1; r <= rounds; ++r) {
            /* 与驱动线程对齐轮次：投递落在等待之前或之内都算，两者都必须被
               同一轮等待消化。 */
            while (gate.load(std::memory_order_acquire) < r)
                std::this_thread::yield();

            boost::asio::post(poller.GetExecutor(), [&, r] {
                if (std::this_thread::get_id() == driver_tid)
                    handled_on_driver.fetch_add(1);
                handled.store(r, std::memory_order_release);
            });
        }
    });

    for (int r = 1; r <= rounds; ++r) {
        gate.store(r, std::memory_order_release);

        const auto begin = std::chrono::steady_clock::now();
        const bool dispatched = poller.PollOnceUntil(wait_deadline);
        const auto elapsed = std::chrono::steady_clock::now() - begin;

        /* 丢唤醒的形态：本轮等满到 deadline、返回 false、handled 停在上轮。 */
        BOOST_CHECK(dispatched);
        BOOST_CHECK_EQUAL(handled.load(std::memory_order_acquire), r);
        BOOST_CHECK(elapsed < std::chrono::seconds(1));
    }

    poster.join();
    BOOST_CHECK_EQUAL(handled_on_driver.load(), rounds);
    /* 锚点 timer 未到期：上面的返回都来自 post/fd 唤醒，不是 deadline 到期。 */
    BOOST_CHECK_EQUAL(anchor_fired.load(), 0);
}

/* 空 context：等到 deadline 才返回，不提前（不忙轮询）、不挂死；之后仍可复用。 */
BOOST_AUTO_TEST_CASE(t_wait_empty_context_sleeps_then_reusable)
{
    CoPoller poller;

    const auto begin = std::chrono::steady_clock::now();
    const bool dispatched = poller.PollOnceUntil(begin + std::chrono::milliseconds(80));
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    BOOST_CHECK(!dispatched);
    /* 下界＝没有已注册事件时必须真的等（错误形态 b：立即返回成忙轮询）。
       留 20ms 余量，只排除「近似瞬时返回」。 */
    BOOST_CHECK(elapsed >= std::chrono::milliseconds(60));
    BOOST_CHECK(elapsed < kBoundedWaitCeiling);

    /* 复用：空等之后的下一轮必须能注册并派发真实事件。 */
    std::atomic<int> fired{0};
    auto timer = MakeTimerEvent(poller, fired, 1);
    BOOST_REQUIRE(timer != nullptr);

    const auto reuse_begin = std::chrono::steady_clock::now();
    const bool reuse_dispatched = poller.PollOnceUntil(reuse_begin + std::chrono::seconds(2));

    BOOST_CHECK(reuse_dispatched);
    BOOST_CHECK_EQUAL(fired.load(), 1);
    BOOST_CHECK(std::chrono::steady_clock::now() - reuse_begin < std::chrono::seconds(1));
}

/* 过期 deadline：不阻塞、不丢失事件；随后的非阻塞 poll 必须照常兑现。 */
BOOST_AUTO_TEST_CASE(t_wait_expired_deadline_is_nonblocking)
{
    CoPoller poller;

    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);
    const char byte = 'x';
    BOOST_REQUIRE_EQUAL(::write(fds[1], &byte, 1), 1);

    std::atomic<int> fired{0};
    auto event = MakeReadableEvent(poller, fds[0], fired);
    BOOST_REQUIRE(event != nullptr);

    const auto begin = std::chrono::steady_clock::now();
    poller.PollOnceUntil(begin - std::chrono::milliseconds(1));
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    /* 只断言「非阻塞」，不锁定过期 deadline 是否顺手派发（两者都合法）。 */
    BOOST_CHECK(elapsed < std::chrono::milliseconds(50));

    /* 事件不能因为过期 deadline 的那次调用而丢失。 */
    poller.PollOnce();
    BOOST_CHECK_EQUAL(fired.load(), 1);

    ::close(fds[0]);
    ::close(fds[1]);
}

/* 持续 ready：每轮等待都必须在 deadline+余量内返回，维护节拍不被事件堆推迟。 */
BOOST_AUTO_TEST_CASE(t_wait_sustained_ready_keeps_deadline)
{
    CoPoller poller;

    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);

    std::atomic<int> fired{0};
    auto event = MakeReadableEvent(poller, fds[0], fired);
    BOOST_REQUIRE(event != nullptr);

    const int rounds = 8;
    /* 每轮共用同一 deadline：提前唤醒后截止等待仍挂在原 deadline 上，故每轮
       返回都必须来自 fd 派发，而不是重新 arm 的边界效应。 */
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    const auto all_begin = std::chrono::steady_clock::now();

    for (int r = 1; r <= rounds; ++r) {
        /* 每轮重新登记一次性读事件并灌一个新字节：形成持续 ready 流。 */
        const char byte = 'x';
        BOOST_REQUIRE_EQUAL(::write(fds[1], &byte, 1), 1);
        if (r > 1)
            BOOST_REQUIRE_EQUAL(event->StartListen(0), 0);

        const auto begin = std::chrono::steady_clock::now();
        const bool dispatched = poller.PollOnceUntil(wait_deadline);
        const auto elapsed = std::chrono::steady_clock::now() - begin;

        BOOST_CHECK(dispatched);
        BOOST_CHECK_EQUAL(fired.load(), r);
        /* 单轮上界：只判「不会因为持续 ready 而无界推迟」，不是性能断言。 */
        BOOST_CHECK(elapsed < std::chrono::seconds(1));
    }

    /* 整体上界：持续 ready 不该把停拍维护一直推后（这里是 fail-safe，不是延迟测量）。 */
    BOOST_CHECK(std::chrono::steady_clock::now() - all_begin < std::chrono::seconds(2));

    ::close(fds[0]);
    ::close(fds[1]);
}

/* 驱动所有权、in_poll 保护与延期销毁在等待路径上必须与 PollOnce 一致。 */
BOOST_AUTO_TEST_CASE(t_wait_keeps_driver_and_in_poll_guard)
{
    CoPoller poller;
    const auto driver_tid = std::this_thread::get_id();
    const uint64_t off_driver_before = poller.GetDrainOffDriverCount();

    std::atomic<bool> handler_on_driver{false};
    std::atomic<uint64_t> in_poll_before{0};
    std::atomic<uint64_t> in_poll_after{0};
    std::atomic<int> fired{0};

    auto timer = poller.CreateEvent(-1, bbt::pollevent::EventOpt::TIMEOUT,
        [&](int, short, bbt::pollevent::EventId) {
            handler_on_driver.store(std::this_thread::get_id() == driver_tid);
            in_poll_before.store(poller.GetDrainInPollCount());
            /* handler 内嵌套 flush：等待期间必须与 poll 期间同样 no-op + 计数，
               不能提前兑现延迟析构。 */
            poller.FlushDeferredEvents();
            in_poll_after.store(poller.GetDrainInPollCount());
            fired.fetch_add(1);
        });
    BOOST_REQUIRE(timer != nullptr);
    BOOST_REQUIRE_EQUAL(timer->StartListen(20), 0);

    /* 进入等待前必须兑现积压：非驱动线程 flush 是 no-op，等待起点即释放。 */
    auto deferred = poller.CreateEvent(-1, bbt::pollevent::EventOpt::TIMEOUT,
        [](int, short, bbt::pollevent::EventId) {});
    BOOST_REQUIRE(deferred != nullptr);
    std::weak_ptr<bbt::pollevent::Event> deferred_weak = deferred;
    poller.DeferDestroyEvent(std::move(deferred));

    /* 等待窗口内的非驱动线程 flush：仍 no-op，只计数，不析构 Event。 */
    std::atomic<bool> flush_go{false};
    std::thread off_driver_flush([&] {
        while (!flush_go.load(std::memory_order_acquire))
            std::this_thread::yield();
        poller.FlushDeferredEvents();
    });
    flush_go.store(true, std::memory_order_release);

    const bool dispatched = poller.PollOnceUntil(std::chrono::steady_clock::now() + std::chrono::seconds(2));
    off_driver_flush.join();

    BOOST_CHECK(dispatched);
    BOOST_CHECK_EQUAL(fired.load(), 1);
    BOOST_CHECK(handler_on_driver.load());
    BOOST_CHECK(in_poll_after.load() > in_poll_before.load());
    BOOST_CHECK(deferred_weak.expired());
    BOOST_CHECK(poller.GetDrainOffDriverCount() > off_driver_before);
}

/* handler 抛异常：异常照旧外泄，in_poll 必须复位，后续等待仍可用。 */
BOOST_AUTO_TEST_CASE(t_wait_handler_exception_resets_in_poll)
{
    CoPoller poller;
    const auto driver_tid = std::this_thread::get_id();

    auto throwing = poller.CreateEvent(-1, bbt::pollevent::EventOpt::TIMEOUT,
        [](int, short, bbt::pollevent::EventId) {
            throw std::runtime_error{"handler throws"};
        });
    BOOST_REQUIRE(throwing != nullptr);
    BOOST_REQUIRE_EQUAL(throwing->StartListen(1), 0);

    /* 两次等待共用同一 deadline：异常路径不注销未到期的截止等待，下一轮复用
       同一条，可避免「重新 arm」本身带来的额外派发干扰断言。 */
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    bool caught = false;
    try
    {
        poller.PollOnceUntil(wait_deadline);
    }
    catch (const std::runtime_error&)
    {
        caught = true;
    }
    BOOST_CHECK(caught);

    /* in_poll 未复位时这里会被计成「poll 内调用」而不兑现。 */
    const uint64_t in_poll_before = poller.GetDrainInPollCount();
    poller.FlushDeferredEvents();
    BOOST_CHECK_EQUAL(poller.GetDrainInPollCount(), in_poll_before);

    /* 驱动 thread id 未被异常路径污染，同线程下一轮仍可等待并派发。 */
    std::atomic<int> fired{0};
    auto timer = MakeTimerEvent(poller, fired, 1);
    BOOST_REQUIRE(timer != nullptr);
    const bool dispatched = poller.PollOnceUntil(wait_deadline);
    BOOST_CHECK(dispatched);
    BOOST_CHECK_EQUAL(fired.load(), 1);
    BOOST_CHECK(std::this_thread::get_id() == driver_tid);
}

/* 截止等待不再注册独立 timer：FD 析构的 poll 冲刷后，另一个未就绪 FD
 * 仍可使 context 保持有工作。本例验证这时等待按 deadline 返回且 FD 可继续使用；
 * 不模拟异线程 restart，也不声称复现已删除的截止 handler 被消费的精确交错。 */
BOOST_AUTO_TEST_CASE(t_wait_deadline_after_fd_destruction_flush)
{
    CoPoller poller;
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);
    Defer close_fds([&] {
        ::close(fds[0]);
        ::close(fds[1]);
    });

    std::atomic<int> fired{0};
    auto pending = MakeReadableEvent(poller, fds[0], fired);
    BOOST_REQUIRE(pending != nullptr);
    auto cancelled = MakeReadableEvent(poller, fds[0], fired);
    BOOST_REQUIRE(cancelled != nullptr);
    cancelled.reset();
    BOOST_CHECK_EQUAL(poller.GetEventLoop()->GetEventNum(), 1);

    const auto begin = std::chrono::steady_clock::now();
    BOOST_CHECK(!poller.PollOnceUntil(begin + std::chrono::milliseconds(80)));
    BOOST_CHECK(std::chrono::steady_clock::now() - begin < kBoundedWaitCeiling);
    BOOST_CHECK_EQUAL(fired.load(), 0);

    const char byte = 'x';
    BOOST_REQUIRE_EQUAL(::write(fds[1], &byte, 1), 1);
    BOOST_CHECK(poller.PollOnceUntil(std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    BOOST_CHECK_EQUAL(fired.load(), 1);
}

/* 对象数非零但没有 pending wait：截止点前不忙轮询，context stopped 后仍可复用。 */
BOOST_AUTO_TEST_CASE(t_wait_unarmed_event_sleeps_and_context_is_reusable)
{
    CoPoller poller;
    auto unarmed = poller.CreateEvent(-1, bbt::pollevent::EventOpt::TIMEOUT,
        [](int, short, bbt::pollevent::EventId) {});
    BOOST_REQUIRE(unarmed != nullptr);
    BOOST_CHECK(!poller.PollOnce());

    const auto begin = std::chrono::steady_clock::now();
    BOOST_CHECK(!poller.PollOnceUntil(begin + std::chrono::milliseconds(80)));
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    BOOST_CHECK(elapsed >= std::chrono::milliseconds(60));
    BOOST_CHECK(elapsed < kBoundedWaitCeiling);

    bool handled = false;
    boost::asio::post(poller.GetExecutor(), [&handled] { handled = true; });
    BOOST_CHECK(poller.PollOnceUntil(std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    BOOST_CHECK(handled);
}

BOOST_AUTO_TEST_SUITE_END()
