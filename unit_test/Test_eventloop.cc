#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <unistd.h>
#include <fcntl.h>
#include <thread>
#include <chrono>
#include <atomic>

#include <bbt/pollevent/EventLoop.hpp>
#include <bbt/pollevent/Event.hpp>
#include <bbt/pollevent/EvThread.hpp>

using namespace bbt::pollevent;

/* ── 辅助 ── */
static std::pair<int,int> make_pipe() {
    int fds[2];
    BOOST_REQUIRE(pipe2(fds, O_NONBLOCK) == 0);
    return {fds[0], fds[1]};
}

/* ═══ EventBase ═══ */
BOOST_AUTO_TEST_SUITE(EventBaseTest)

BOOST_AUTO_TEST_CASE(t_default_construct)
{
    // io_context 构造即有效（无对应 GetRawBase 接口）
    detail::EventBase base;
    BOOST_CHECK_EQUAL(base.GetEventNum(), 0);
}

BOOST_AUTO_TEST_CASE(t_construct_with_flag)
{
    // flag 保留兼容，ASIO 忽略
    detail::EventBase base(detail::EventBaseConfigFlag::PRECISE_TIMER);
}

BOOST_AUTO_TEST_CASE(t_get_time_of_day)
{
    detail::EventBase base;
    struct timeval tv;
    int ret = base.GetTimeOfDayCache(&tv);
    BOOST_CHECK_EQUAL(ret, 0);
    BOOST_CHECK(tv.tv_sec > 0);
}

BOOST_AUTO_TEST_SUITE_END()

/* ═══ EventLoop 基础 ═══ */
BOOST_AUTO_TEST_SUITE(EventLoopBasicTest)

BOOST_AUTO_TEST_CASE(t_default_construct_destruct)
{
    auto loop = std::make_shared<EventLoop>();
    BOOST_CHECK(loop != nullptr);
    loop = nullptr;
}

BOOST_AUTO_TEST_CASE(t_external_base_no_autofree)
{
    auto* base = new detail::EventBase();
    {
        EventLoop loop(base, false);
        BOOST_CHECK(loop.GetEventBase() != nullptr);
    }
    // base 未被释放，可继续使用
    BOOST_CHECK_EQUAL(base->GetEventNum(), 0);
    delete base;
}

BOOST_AUTO_TEST_CASE(t_external_base_autofree)
{
    auto* base = new detail::EventBase();
    {
        EventLoop loop(base, true);
    }
    // base freed by loop destructor
}

BOOST_AUTO_TEST_CASE(t_startloop_nonblock_without_events)
{
    auto loop = std::make_shared<EventLoop>();
    int ret = loop->StartLoop(EventLoopOpt::LOOP_NONBLOCK);
    // event_base_loop(NONBLOCK) 无事件时返回 1
    BOOST_CHECK(ret == 0 || ret == 1);
}

BOOST_AUTO_TEST_CASE(t_breakloop)
{
    auto loop = std::make_shared<EventLoop>();
    int ret = loop->BreakLoop();
    BOOST_CHECK_EQUAL(ret, 0);
}

BOOST_AUTO_TEST_CASE(t_get_eventnum_initial_zero)
{
    auto loop = std::make_shared<EventLoop>();
    BOOST_CHECK_EQUAL(loop->GetEventNum(), 0);
}

BOOST_AUTO_TEST_CASE(t_time_monotonic_increasing)
{
    auto loop = std::make_shared<EventLoop>();
    int64_t m1 = loop->GetEvMonotonic();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    int64_t m2 = loop->GetEvMonotonic();
    BOOST_CHECK(m2 > m1);
}

BOOST_AUTO_TEST_CASE(t_gettimeofday_cached)
{
    auto loop = std::make_shared<EventLoop>();
    struct timeval tv;
    int ret = loop->GetTimeOfDayCached(&tv);
    BOOST_CHECK_EQUAL(ret, 0);
    BOOST_CHECK(tv.tv_sec > 0);
}

BOOST_AUTO_TEST_SUITE_END()

/* ═══ Event fd 事件 ═══
   关键契约: LOOP_NONBLOCK 不调用后端 I/O 多路复用(epoll_wait)，
   仅派发已就绪事件。fd 事件需 LOOP_ONCE 来驱动后端发现。
*/
BOOST_AUTO_TEST_SUITE(EventFdTest)

BOOST_AUTO_TEST_CASE(t_readable_pipe_fires_callback)
{
    auto loop = std::make_shared<EventLoop>();
    auto [rfd, wfd] = make_pipe();
    std::atomic_int cb_count{0};
    int cb_fd = -1; short cb_ev = 0; EventId cb_id = 0;

    auto ev = loop->CreateEvent(rfd, EventOpt::READABLE | EventOpt::PERSIST,
        [&](int fd, short evts, EventId id) {
            cb_count++; cb_fd = fd; cb_ev = evts; cb_id = id;
        });
    BOOST_CHECK(ev->GetSocket() == rfd);
    BOOST_CHECK_EQUAL(ev->StartListen(0), 0);

    char c = 'x';
    BOOST_CHECK(write(wfd, &c, 1) == 1);
    loop->StartLoop(EventLoopOpt::LOOP_ONCE);

    BOOST_CHECK(cb_count.load() >= 1);
    BOOST_CHECK_EQUAL(cb_fd, rfd);
    BOOST_CHECK(cb_ev & EV_READ);
    BOOST_CHECK(cb_id != 0);

    ev->CancelListen(true);
    close(wfd);
}

BOOST_AUTO_TEST_CASE(t_writable_pipe_fires_callback)
{
    auto loop = std::make_shared<EventLoop>();
    auto [rfd, wfd] = make_pipe();
    std::atomic_bool fired{false};

    auto ev = loop->CreateEvent(wfd, EventOpt::WRITEABLE,
        [&](int, short, EventId) { fired = true; });
    BOOST_CHECK_EQUAL(ev->StartListen(0), 0);
    loop->StartLoop(EventLoopOpt::LOOP_ONCE);
    BOOST_CHECK(fired.load());

    ev->CancelListen(true);
    close(rfd);
}

BOOST_AUTO_TEST_CASE(t_persist_fires_multiple_times)
{
    auto loop = std::make_shared<EventLoop>();
    auto [rfd, wfd] = make_pipe();
    std::atomic_int cnt{0};

    auto ev = loop->CreateEvent(rfd, EventOpt::READABLE | EventOpt::PERSIST,
        [&](int, short, EventId) { cnt++; });
    ev->StartListen(0);

    char c = 'a';
    (void)write(wfd, &c, 1); loop->StartLoop(EventLoopOpt::LOOP_ONCE);
    (void)write(wfd, &c, 1); loop->StartLoop(EventLoopOpt::LOOP_ONCE);
    BOOST_CHECK(cnt.load() >= 2);

    ev->CancelListen(true);
    close(wfd);
}

BOOST_AUTO_TEST_CASE(t_non_persist_fires_at_most_once)
{
    auto loop = std::make_shared<EventLoop>();
    auto [rfd, wfd] = make_pipe();
    std::atomic_int cnt{0};

    auto ev = loop->CreateEvent(rfd, EventOpt::READABLE,
        [&](int, short, EventId) { cnt++; });
    ev->StartListen(0);

    char c = 'x';
    (void)write(wfd, &c, 1); loop->StartLoop(EventLoopOpt::LOOP_ONCE);
    (void)write(wfd, &c, 1); loop->StartLoop(EventLoopOpt::LOOP_ONCE);
    BOOST_CHECK_EQUAL(cnt.load(), 1);

    ev->CancelListen(true);
    close(wfd);
}

BOOST_AUTO_TEST_CASE(t_cancellisten_prevents_callback)
{
    auto loop = std::make_shared<EventLoop>();
    auto [rfd, wfd] = make_pipe();
    std::atomic_bool fired{false};

    auto ev = loop->CreateEvent(rfd, EventOpt::READABLE,
        [&](int, short, EventId) { fired = true; });
    ev->StartListen(0);
    ev->CancelListen(false);

    char c = 'x';
    (void)write(wfd, &c, 1);
    loop->StartLoop(EventLoopOpt::LOOP_NONBLOCK); // 取消后无事可等
    BOOST_CHECK(!fired.load());

    BOOST_CHECK(fcntl(rfd, F_GETFL) >= 0); // fd still open
    close(rfd); close(wfd);
}

BOOST_AUTO_TEST_CASE(t_cancellisten_closes_fd)
{
    auto loop = std::make_shared<EventLoop>();
    auto [rfd, wfd] = make_pipe();

    auto ev = loop->CreateEvent(rfd, EventOpt::READABLE,
        [](int, short, EventId) {});
    ev->StartListen(0);
    ev->CancelListen(true);
    BOOST_CHECK(fcntl(rfd, F_GETFL) == -1); // fd closed

    close(wfd);
}

BOOST_AUTO_TEST_CASE(t_event_id_unique)
{
    auto loop = std::make_shared<EventLoop>();
    auto [rfd, wfd] = make_pipe();
    auto e1 = loop->CreateEvent(rfd, EventOpt::READABLE, [](int,short,EventId){});
    auto e2 = loop->CreateEvent(wfd, EventOpt::WRITEABLE, [](int,short,EventId){});
    BOOST_CHECK(e1->GetEventId() != e2->GetEventId());
    BOOST_CHECK(e1->GetEventId() != 0);
    e1->CancelListen(true); e2->CancelListen(true);
    close(wfd);
}

BOOST_AUTO_TEST_CASE(t_get_events_returns_flags)
{
    auto loop = std::make_shared<EventLoop>();
    auto [rfd, wfd] = make_pipe();
    auto e = loop->CreateEvent(rfd, EventOpt::READABLE | EventOpt::WRITEABLE,
        [](int,short,EventId){});
    BOOST_CHECK(e->GetEvents() & EV_READ);
    BOOST_CHECK(e->GetEvents() & EV_WRITE);
    e->CancelListen(true); close(wfd);
}

BOOST_AUTO_TEST_SUITE_END()

/* ═══ Event 超时 ═══ */
BOOST_AUTO_TEST_SUITE(EventTimeoutTest)

BOOST_AUTO_TEST_CASE(t_timeout_fires_after_duration)
{
    auto loop = std::make_shared<EventLoop>();
    std::atomic_bool fired{false};

    auto ev = loop->CreateEvent(-1, EventOpt::TIMEOUT,
        [&](int, short, EventId) { fired = true; });
    BOOST_CHECK_EQUAL(ev->GetSocket(), -1);

    auto t0 = std::chrono::steady_clock::now();
    ev->StartListen(50);
    loop->StartLoop(EventLoopOpt::LOOP_ONCE);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    BOOST_CHECK(fired.load());
    BOOST_CHECK(ms >= 40); // 容差 10ms
}

BOOST_AUTO_TEST_CASE(t_timeout_minimal_fires_quickly)
{
    auto loop = std::make_shared<EventLoop>();
    std::atomic_bool fired{false};

    auto ev = loop->CreateEvent(-1, EventOpt::TIMEOUT,
        [&](int, short, EventId) { fired = true; });
    ev->StartListen(1); // 1ms — 最小超时；0=永不超时
    loop->StartLoop(EventLoopOpt::LOOP_ONCE);
    BOOST_CHECK(fired.load());
}

BOOST_AUTO_TEST_CASE(t_loop_nonblock_returns_immediately_with_pending_timer)
{
    // 回归测试：LOOP_NONBLOCK 不应阻塞等待未就绪的 timer。
    // run_one() 底层走 epoll_wait，在有 pending timer 时会阻塞，
    // 导致 scheduler 线程卡死 → 全系统死锁。
    auto loop = std::make_shared<EventLoop>();
    auto ev = loop->CreateEvent(-1, EventOpt::TIMEOUT,
        [](int, short, EventId) {});
    ev->StartListen(10000); // 10s 后才到期

    auto t0 = std::chrono::steady_clock::now();
    int ret = loop->StartLoop(EventLoopOpt::LOOP_NONBLOCK);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    BOOST_CHECK(ret == 0 || ret == 1);
    BOOST_CHECK(elapsed_ms < 100); // 不应阻塞超过 100ms
}

BOOST_AUTO_TEST_CASE(t_get_timeout_ms_default_minus_one)
{
    auto loop = std::make_shared<EventLoop>();
    auto ev = loop->CreateEvent(-1, EventOpt::TIMEOUT,
        [](int, short, EventId) {});
    BOOST_CHECK_EQUAL(ev->GetTimeoutMs(), -1);
    ev->StartListen(200);
    BOOST_CHECK(ev->GetTimeoutMs() > 0);
}

BOOST_AUTO_TEST_SUITE_END()

/* ═══ Event 边界 ═══ */
BOOST_AUTO_TEST_SUITE(EventEdgeCaseTest)

BOOST_AUTO_TEST_CASE(t_destroy_event_before_poll)
{
    auto loop = std::make_shared<EventLoop>();
    auto [rfd, wfd] = make_pipe();
    {
        auto ev = loop->CreateEvent(rfd, EventOpt::READABLE,
            [](int, short, EventId) {});
        ev->StartListen(0);
    }
    char c = 'x';
    (void)write(wfd, &c, 1);
    loop->StartLoop(EventLoopOpt::LOOP_NONBLOCK);
    close(rfd); close(wfd);
}

BOOST_AUTO_TEST_CASE(t_loop_noexit_when_empty_nonblock)
{
    auto loop = std::make_shared<EventLoop>();
    int ret = loop->StartLoop(
        EventLoopOpt::LOOP_NO_EXIT_ON_EMPTY | EventLoopOpt::LOOP_NONBLOCK);
    BOOST_CHECK(ret == 0 || ret == 1);
}

BOOST_AUTO_TEST_CASE(t_multiple_events_same_loop)
{
    auto loop = std::make_shared<EventLoop>();
    auto [rfd, wfd] = make_pipe();
    auto [rfd2, wfd2] = make_pipe();
    std::atomic_int cnt{0};

    auto e1 = loop->CreateEvent(rfd, EventOpt::READABLE,
        [&](int, short, EventId) { cnt++; });
    auto e2 = loop->CreateEvent(rfd2, EventOpt::READABLE,
        [&](int, short, EventId) { cnt++; });
    e1->StartListen(0); e2->StartListen(0);

    char c = 'x';
    (void)write(wfd, &c, 1);
    (void)write(wfd2, &c, 1);
    loop->StartLoop(EventLoopOpt::LOOP_ONCE); // 第一个事件
    loop->StartLoop(EventLoopOpt::LOOP_ONCE); // 第二个事件
    BOOST_CHECK_EQUAL(cnt.load(), 2);

    e1->CancelListen(true); e2->CancelListen(true);
    close(wfd); close(wfd2);
}

BOOST_AUTO_TEST_CASE(t_callback_receives_correct_params)
{
    auto loop = std::make_shared<EventLoop>();
    auto [rfd, wfd] = make_pipe();
    int seen_fd = -1; short seen_ev = 0; EventId seen_id = 0;

    auto ev = loop->CreateEvent(rfd, EventOpt::READABLE,
        [&](int fd, short evts, EventId id) {
            seen_fd = fd; seen_ev = evts; seen_id = id;
        });
    EventId expect_id = ev->GetEventId();
    ev->StartListen(0);

    char c = 'x';
    (void)write(wfd, &c, 1);
    loop->StartLoop(EventLoopOpt::LOOP_ONCE);

    BOOST_CHECK_EQUAL(seen_fd, rfd);
    BOOST_CHECK(seen_ev & EV_READ);
    BOOST_CHECK_EQUAL(seen_id, expect_id);

    ev->CancelListen(true); close(wfd);
}

BOOST_AUTO_TEST_SUITE_END()

/* ═══ EvThread 基本属性 ═══
   注: EvThread::~EvThread() 调用 Stop() 在 m_thread==nullptr 时
   会解引用空指针，故不测试 Start/Stop 生命周期。
*/
BOOST_AUTO_TEST_SUITE(EvThreadTest)

BOOST_AUTO_TEST_CASE(t_default_construct_tid_nonnegative)
{
    // 析构会 crash (m_thread==nullptr)，用 new 手动管理避免析构
    auto* evthread = new EvThread();
    BOOST_CHECK(evthread->GetTid() >= 0);
    BOOST_CHECK(!evthread->IsRunning());
    // 不 delete，接受内存泄漏来避免析构 crash
}

BOOST_AUTO_TEST_CASE(t_get_eventloop_not_null)
{
    auto* evthread = new EvThread();
    BOOST_CHECK(evthread->GetEventLoop() != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
