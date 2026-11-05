/**
 * @file Test_event_round.cc
 * @brief Event 监听轮次回归：旧完成回调、重入启动/取消、取消驱动与回调寿命。
 *
 * 时序全部由可控交互制造，不靠 sleep 堆次数：
 *   - 同一方向重复挂载时，一次就绪会完成该方向队列中的全部等待，「重入
 *     StartListen 是否多挂一份」用回调次数直接判别；
 *   - CancelListen 不再排空 context，被取消的完成由下一次 poll 兑现，可在
 *     取消乃至析构之后才到达，用来验证完成回调只持有 weak 句柄；
 *   - 两份 1 ms timer 都在 StartListen 返回前设置 expires_after；返回后再等同一 interval，
 *     证明真实 expiry 已到，再由 run_one 只派发一份，另一份留在成功完成队列；
 *   - StartListen 装配交错用 test-only hook（BBT_POLLEVENT_TESTING，仅 NEED_TEST
 *     构建）：装配线程在临界区内等完成方进入回调后，才继续挂 timer、放锁；
 *   - 旧轮次判定只依赖 Event 自己的轮次号，不依赖线程调度。
 *
 * 修复前基线的判别点：PERSIST 重挂与回调内重入叠加出第二次等待（一次就绪
 * 交付 2 次回调）；CancelListen 内的 restart()+poll() 会在取消方栈上派发其它
 * Event 的回调；栈上 Event 仍被接受进入异步监听。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include <bbt/pollevent/Event.hpp>
#include <bbt/pollevent/detail/EventBase.hpp>

using namespace bbt::pollevent;

namespace {

/* 只驱动已就绪的完成，不做阻塞等待（等价 EventLoopOpt::LOOP_NONBLOCK）。 */
void PollAll(boost::asio::io_context& ctx)
{
    ctx.restart();
    ctx.poll();
}

int CountOpenFds()
{
    int n = 0;
    if (auto* d = ::opendir("/proc/self/fd")) {
        while (::readdir(d) != nullptr)
            ++n;
        ::closedir(d);
    }
    return n;
}

/* 预热 reactor 基建 fd（epoll/eventfd/timerfd），使后续 fd 计数只反映 Event 自身。 */
void WarmupReactor(detail::EventBase& base)
{
    auto timer = std::make_shared<Event>(&base, -1, static_cast<short>(EventOpt::TIMEOUT), [](int, short, EventId) {});
    BOOST_REQUIRE_EQUAL(timer->StartListen(1), 0);
    PollAll(base.GetContext());
}

/* 非阻塞读端：多挂的等待可能在数据已被消费后仍交付一次，回调里的 read 不能阻塞。 */
std::pair<int, int> MakePipe()
{
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);
    BOOST_REQUIRE_EQUAL(::fcntl(fds[0], F_SETFL, O_NONBLOCK), 0);
    BOOST_REQUIRE_EQUAL(::fcntl(fds[1], F_SETFL, O_NONBLOCK), 0);
    return {fds[0], fds[1]};
}

/* socketpair 读端既有数据可读、又始终可写：两个方向的等待都会立即完成。 */
std::pair<int, int> MakeReadySocketPair()
{
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const char byte = 'x';
    BOOST_REQUIRE_EQUAL(::write(fds[1], &byte, 1), 1);
    return {fds[0], fds[1]};
}

/* 两个 fd+EV_TIMEOUT Event 的配对夹具：第一份 timer 成功完成由 run_one 派发，
 * 第二份留在 io_context 队列里（已排队的成功完成）。 */
struct TimerEventPair {
    std::shared_ptr<Event> a;
    std::shared_ptr<Event> b;
    std::atomic<int> hits_a{0};
    std::atomic<int> hits_b{0};
    std::pair<int, int> fds_a{-1, -1};
    std::pair<int, int> fds_b{-1, -1};
};

/* 挂两个 1 ms timer；两次 StartListen 返回后再等待同一 interval，真实 expiry 均已到期，
 * run_one 的 reactor 收集成功完成，只派发其中一份。 */
void ArmQueuedTimerPair(detail::EventBase& base, TimerEventPair& p)
{
    p.fds_a = MakePipe();
    p.fds_b = MakePipe();

    const short events = static_cast<short>(EventOpt::READABLE | EventOpt::TIMEOUT);
    p.a = std::make_shared<Event>(&base, p.fds_a.first, events, [&p](int, short, EventId) { p.hits_a.fetch_add(1); });
    p.b = std::make_shared<Event>(&base, p.fds_b.first, events, [&p](int, short, EventId) { p.hits_b.fetch_add(1); });

    constexpr uint64_t timeout_ms = 1;
    constexpr auto interval = std::chrono::milliseconds(timeout_ms);
    BOOST_REQUIRE_EQUAL(p.a->StartListen(timeout_ms), 0);
    BOOST_REQUIRE_EQUAL(p.b->StartListen(timeout_ms), 0);

    /* 两次 StartListen 均已返回后，再等待同一 interval；真实 expiry 已在
     * StartListen 内设置，因此到此时两份 timer 都已到期。 */
    const auto expiry_bound = std::chrono::steady_clock::now() + interval;
    std::this_thread::sleep_until(expiry_bound);

    base.GetContext().restart();
    BOOST_REQUIRE_EQUAL(base.GetContext().run_one(), 1);
    BOOST_REQUIRE_EQUAL(p.hits_a.load() + p.hits_b.load(), 1);
}

/* 装配/完成交错同步点：hook 由 Event 在持锁（装配方）或取锁前（完成方）调用，
 * 这里只做线程同步，不重入 Event。 */
struct InterleaveGate {
    std::mutex m;
    std::condition_variable cv;
    bool driver_ready{false};
    bool completion_entered{false};
    bool overlap{false};
    bool delivered{false};
};

InterleaveGate* g_interleave_gate = nullptr;

void InterleaveHook(Event*, Event::TestHookPoint point)
{
    InterleaveGate* gate = g_interleave_gate;
    if (gate == nullptr)
        return;

    if (point == Event::TestHookPoint::FdCompletionEnter) {
        {
            std::lock_guard<std::mutex> lock(gate->m);
            gate->completion_entered = true;
        }
        gate->cv.notify_all();
        return;
    }

    /* 装配方：等完成方进入回调（随后它会阻塞在 m_mutex 上），再继续挂 timer。 */
    std::unique_lock<std::mutex> lock(gate->m);
    gate->overlap = gate->cv.wait_for(lock, std::chrono::seconds(5), [gate] { return gate->completion_entered; });
}

} // namespace

BOOST_AUTO_TEST_SUITE(EventRoundTest)

/* 重新 StartListen 后只保留本轮挂载：旧轮次的等待被撤销，一次就绪只交付一次。 */
BOOST_AUTO_TEST_CASE(t_restart_listen_keeps_single_round)
{
    detail::EventBase base;
    WarmupReactor(base);
    auto [rfd, wfd] = MakeReadySocketPair();

    std::atomic<int> hits{0};
    auto ev = std::make_shared<Event>(&base, rfd, static_cast<short>(EventOpt::READABLE | EventOpt::WRITEABLE),
                                      [&hits](int, short, EventId) { hits.fetch_add(1); });

    BOOST_REQUIRE_EQUAL(ev->StartListen(0), 0);
    BOOST_REQUIRE_EQUAL(ev->StartListen(0), 0); // 新一轮覆盖上一轮

    PollAll(base.GetContext());
    BOOST_CHECK_EQUAL(hits.load(), 2); // 两个方向各一次

    ::close(rfd);
    ::close(wfd);
}

/* 回调内重入 StartListen：旧回调返回后不得再补挂一份等待。
 * 修复前：一次就绪完成两份等待，第二个字节交付 2 次回调（累计 3）。 */
BOOST_AUTO_TEST_CASE(t_reentrant_startlisten_does_not_duplicate_wait)
{
    detail::EventBase base;
    auto [rfd, wfd] = MakePipe();

    std::atomic<int> hits{0};
    std::shared_ptr<Event> ev;
    ev = std::make_shared<Event>(&base, rfd, static_cast<short>(EventOpt::READABLE | EventOpt::PERSIST),
                                 [&hits, &ev](int fd, short, EventId) {
                                     char byte = '\0';
                                     (void)::read(fd, &byte, 1); // 先消费就绪数据，重挂时不带就绪
                                     if (hits.fetch_add(1) == 0)
                                         ev->StartListen(0); // 回调内启动新一轮
                                 });

    BOOST_REQUIRE_EQUAL(ev->StartListen(0), 0);
    const char byte = 'x';
    BOOST_REQUIRE_EQUAL(::write(wfd, &byte, 1), 1);
    PollAll(base.GetContext());
    BOOST_CHECK_EQUAL(hits.load(), 1);

    /* 无数据时空转：多挂的那份等待不得自己交付。 */
    PollAll(base.GetContext());
    BOOST_CHECK_EQUAL(hits.load(), 1);

    BOOST_REQUIRE_EQUAL(::write(wfd, &byte, 1), 1);
    PollAll(base.GetContext());
    BOOST_CHECK_EQUAL(hits.load(), 2);

    ::close(rfd);
    ::close(wfd);
}

/* 回调内释放最后一个外部引用：PERSIST 不得在已析构对象上重挂。
 * 回调执行期间由 weak.lock() 的局部强引用保活，引用归零发生在其返回之后。 */
BOOST_AUTO_TEST_CASE(t_release_in_callback_ignores_persist_rearm)
{
    detail::EventBase base;
    auto [rfd, wfd] = MakePipe();

    std::atomic<int> hits{0};
    std::shared_ptr<Event> ev;
    std::weak_ptr<Event> weak_ev;
    ev = std::make_shared<Event>(&base, rfd, static_cast<short>(EventOpt::READABLE | EventOpt::PERSIST),
                                 [&hits, &ev](int fd, short, EventId) {
                                     char byte = '\0';
                                     (void)::read(fd, &byte, 1);
                                     hits.fetch_add(1);
                                     ev.reset(); // 回调内释放最后一个外部引用
                                 });
    weak_ev = ev;

    BOOST_REQUIRE_EQUAL(ev->StartListen(0), 0);
    const char byte = 'x';
    BOOST_REQUIRE_EQUAL(::write(wfd, &byte, 1), 1);
    PollAll(base.GetContext());
    BOOST_CHECK_EQUAL(hits.load(), 1);
    BOOST_CHECK(weak_ev.expired()); // 回调返回后对象已释放

    BOOST_REQUIRE_EQUAL(::write(wfd, &byte, 1), 1);
    PollAll(base.GetContext()); // 无主回调：不崩溃、不交付
    BOOST_CHECK_EQUAL(hits.load(), 1);

    ::close(rfd);
    ::close(wfd);
}

/* CancelListen 不得驱动 io_context：取消时其它 Event 的就绪完成不能被它顺带派发。
 * 修复前：CancelListen 内的 restart()+poll() 排空会在取消方栈上执行别的回调。 */
BOOST_AUTO_TEST_CASE(t_cancellisten_does_not_drive_context)
{
    detail::EventBase base;
    auto [rfd_a, wfd_a] = MakePipe();
    auto [rfd_b, wfd_b] = MakePipe();

    bool in_cancel = false;
    bool other_dispatched_inside_cancel = false;
    std::atomic<int> other_hits{0};

    auto ev_a = std::make_shared<Event>(&base, rfd_a, static_cast<short>(EventOpt::READABLE), [&](int, short, EventId) {
        other_hits.fetch_add(1);
        if (in_cancel)
            other_dispatched_inside_cancel = true;
    });
    auto ev_b =
        std::make_shared<Event>(&base, rfd_b, static_cast<short>(EventOpt::READABLE), [](int, short, EventId) {});

    BOOST_REQUIRE_EQUAL(ev_a->StartListen(0), 0);
    BOOST_REQUIRE_EQUAL(ev_b->StartListen(0), 0);
    const char byte = 'x';
    BOOST_REQUIRE_EQUAL(::write(wfd_a, &byte, 1), 1);
    BOOST_REQUIRE_EQUAL(::write(wfd_b, &byte, 1), 1);

    in_cancel = true;
    BOOST_REQUIRE_EQUAL(ev_b->CancelListen(), 0);
    in_cancel = false;

    BOOST_CHECK(!other_dispatched_inside_cancel); // 取消期间未派发别的回调
    BOOST_CHECK_EQUAL(other_hits.load(), 0);

    PollAll(base.GetContext()); // 之后由驱动方正常派发
    BOOST_CHECK_EQUAL(other_hits.load(), 1);

    ::close(rfd_a);
    ::close(wfd_a);
    ::close(rfd_b);
    ::close(wfd_b);
}

/* 取消后立刻释放：已排队的完成回调不得触碰已析构对象，内部 dup fd 归还。 */
BOOST_AUTO_TEST_CASE(t_queued_completion_after_cancel_and_destroy)
{
    detail::EventBase base;
    WarmupReactor(base);
    auto [rfd, wfd] = MakeReadySocketPair();

    std::atomic<int> hits{0};
    const int fds_before = CountOpenFds();
    {
        auto ev = std::make_shared<Event>(&base, rfd, static_cast<short>(EventOpt::READABLE | EventOpt::WRITEABLE),
                                          [&hits](int, short, EventId) { hits.fetch_add(1); });
        BOOST_REQUIRE_EQUAL(ev->StartListen(0), 0);
        BOOST_REQUIRE_EQUAL(ev->CancelListen(), 0);
    }
    BOOST_CHECK_EQUAL(CountOpenFds(), fds_before); // 内部 dup fd 已释放

    PollAll(base.GetContext()); // 排队完成此刻无主：丢弃
    BOOST_CHECK_EQUAL(hits.load(), 0);

    ::close(rfd);
    ::close(wfd);
}

/* 回调内取消 + PERSIST：旧回调返回后不得重挂复活已取消的监听。 */
BOOST_AUTO_TEST_CASE(t_persist_cancel_in_callback_does_not_resurrect)
{
    detail::EventBase base;
    auto [rfd, wfd] = MakePipe();

    std::atomic<int> hits{0};
    std::shared_ptr<Event> ev;
    const size_t cb_before = Event::CallbackEntryCount();
    ev = std::make_shared<Event>(&base, rfd, static_cast<short>(EventOpt::READABLE | EventOpt::PERSIST),
                                 [&hits, &ev](int fd, short, EventId) {
                                     char byte = '\0';
                                     (void)::read(fd, &byte, 1);
                                     hits.fetch_add(1);
                                     ev->CancelListen(); // 回调内重入取消
                                 });

    BOOST_REQUIRE_EQUAL(ev->StartListen(0), 0);
    const char byte = 'x';
    BOOST_REQUIRE_EQUAL(::write(wfd, &byte, 1), 1);
    PollAll(base.GetContext());
    BOOST_CHECK_EQUAL(hits.load(), 1);
    BOOST_CHECK_EQUAL(Event::CallbackEntryCount(), cb_before); // 回调已摘除

    BOOST_REQUIRE_EQUAL(::write(wfd, &byte, 1), 1); // 数据仍在管道里
    PollAll(base.GetContext());
    BOOST_CHECK_EQUAL(hits.load(), 1); // 不复活

    ::close(rfd);
    ::close(wfd);
}

/* Trigger 的交付携带轮次：投递后取消，回调不得在取消之后才落地。 */
BOOST_AUTO_TEST_CASE(t_two_queued_triggers_are_both_delivered)
{
    detail::EventBase base;
    std::atomic<int> hits{0};
    std::atomic<int> flags{0};
    auto ev = std::make_shared<Event>(&base, -1, 0, [&hits, &flags](int, short flag, EventId) {
        flags.fetch_or(flag);
        hits.fetch_add(1);
    });

    BOOST_REQUIRE_EQUAL(ev->StartListen(0), 0);
    BOOST_REQUIRE_EQUAL(ev->Trigger(EventOpt::READABLE), 0);
    BOOST_REQUIRE_EQUAL(ev->Trigger(EventOpt::WRITEABLE), 0);
    PollAll(base.GetContext());
    BOOST_CHECK_EQUAL(hits.load(), 2);
    BOOST_CHECK_EQUAL(flags.load(), EventOpt::READABLE | EventOpt::WRITEABLE);

    BOOST_REQUIRE_EQUAL(ev->Trigger(EventOpt::READABLE), 0);
    BOOST_REQUIRE_EQUAL(ev->StartListen(0), 0);
    PollAll(base.GetContext());
    BOOST_CHECK_EQUAL(hits.load(), 2);
}

BOOST_AUTO_TEST_CASE(t_trigger_after_cancel_not_delivered)
{
    detail::EventBase base;
    std::atomic<int> hits{0};
    auto ev = std::make_shared<Event>(&base, -1, 0, [&hits](int, short, EventId) { hits.fetch_add(1); });

    BOOST_REQUIRE_EQUAL(ev->StartListen(0), 0);
    BOOST_REQUIRE_EQUAL(ev->Trigger(EventOpt::READABLE), 0);
    PollAll(base.GetContext());
    BOOST_CHECK_EQUAL(hits.load(), 1); // 正例：投递确实交付

    BOOST_REQUIRE_EQUAL(ev->Trigger(EventOpt::READABLE), 0);
    BOOST_REQUIRE_EQUAL(ev->CancelListen(), 0);
    PollAll(base.GetContext());
    BOOST_CHECK_EQUAL(hits.load(), 1); // 取消后落地的那次被丢弃
}

/* 栈上 Event 不能进入监听：异步登记要求 shared ownership 才能保活；
 * 构造、取消与析构仍是合法路径（Test_event_constructor_rollback 依赖）。 */
BOOST_AUTO_TEST_CASE(t_stack_event_startlisten_rejected)
{
    detail::EventBase base;
    auto [rfd, wfd] = MakePipe();
    const size_t cb_before = Event::CallbackEntryCount();

    {
        Event ev(&base, rfd, static_cast<short>(EventOpt::READABLE), [](int, short, EventId) {});
        BOOST_CHECK_EQUAL(Event::CallbackEntryCount(), cb_before + 1);
        BOOST_CHECK_EQUAL(ev.StartListen(0), -1);
        BOOST_CHECK_EQUAL(ev.CancelListen(), 0);
        BOOST_CHECK_EQUAL(Event::CallbackEntryCount(), cb_before);
    }
    BOOST_CHECK_EQUAL(Event::CallbackEntryCount(), cb_before);

    ::close(rfd);
    ::close(wfd);
}

/* 已排队的成功完成（不是 operation_aborted）之后取消/释放：两份 timer 同批过期，
 * run_one 派发一份、另一份成功完成留在 io_context 队列；此时取消或析构该 Event，
 * 排队的完成仍会派发一次，但不得再交付回调、不得触碰已析构对象，内部 dup fd 归还。
 * 对照场景不取消，证明队列里的确实是可交付的成功完成。 */
BOOST_AUTO_TEST_CASE(t_queued_timer_success_after_cancel_and_destroy)
{
    detail::EventBase base;
    WarmupReactor(base);
    base.GetContext().run(); // 兑现预热 timer 的完成，run_one 只面对本用例的完成

    /* 对照：不取消时第二份成功完成确实交付。 */
    {
        const int fds_before = CountOpenFds();
        TimerEventPair p;
        ArmQueuedTimerPair(base, p);
        PollAll(base.GetContext());
        BOOST_CHECK_EQUAL(p.hits_a.load() + p.hits_b.load(), 2);

        p.a.reset();
        p.b.reset();
        ::close(p.fds_a.first);
        ::close(p.fds_a.second);
        ::close(p.fds_b.first);
        ::close(p.fds_b.second);
        BOOST_CHECK_EQUAL(CountOpenFds(), fds_before);
    }

    /* 取消存活对象：排队的成功完成被派发，但轮次已作废，不再交付。 */
    {
        const int fds_before = CountOpenFds();
        TimerEventPair p;
        ArmQueuedTimerPair(base, p);
        std::shared_ptr<Event>& queued = (p.hits_a.load() == 1) ? p.b : p.a;
        BOOST_REQUIRE_EQUAL(queued->CancelListen(), 0);

        PollAll(base.GetContext());
        BOOST_CHECK_EQUAL(p.hits_a.load() + p.hits_b.load(), 1);

        p.a.reset();
        p.b.reset();
        ::close(p.fds_a.first);
        ::close(p.fds_a.second);
        ::close(p.fds_b.first);
        ::close(p.fds_b.second);
        BOOST_CHECK_EQUAL(CountOpenFds(), fds_before);
    }

    /* 释放最后一个引用：排队的成功完成只能拿到失效 weak 句柄。 */
    {
        const int fds_before = CountOpenFds();
        TimerEventPair p;
        ArmQueuedTimerPair(base, p);
        std::shared_ptr<Event>& queued = (p.hits_a.load() == 1) ? p.b : p.a;
        queued.reset();

        PollAll(base.GetContext());
        BOOST_CHECK_EQUAL(p.hits_a.load() + p.hits_b.load(), 1);

        p.a.reset();
        p.b.reset();
        ::close(p.fds_a.first);
        ::close(p.fds_a.second);
        ::close(p.fds_b.first);
        ::close(p.fds_b.second);
        BOOST_CHECK_EQUAL(CountOpenFds(), fds_before);
    }
}

/* StartListen 装配期间 fd 已就绪：装配线程在临界区内挂好 fd 等待后停住，驱动线程
 * 立即完成该等待并阻塞在 m_mutex 上；装配线程随后才挂 timer、放锁。结果只能交付
 * 一次 fd 回调，timer 必须被撤销（否则 join 后 hits 变 2）。 */
BOOST_AUTO_TEST_CASE(t_startlisten_assembly_races_ready_fd)
{
    detail::EventBase base;
    WarmupReactor(base);
    base.GetContext().run(); // 兑现预热 timer 的完成

    auto [rfd, wfd] = MakeReadySocketPair(); // StartListen 之前 fd 已就绪

    InterleaveGate gate;
    g_interleave_gate = &gate;
    Event::SetTestHook(&InterleaveHook);

    std::atomic<int> hits{0};
    std::atomic<short> seen_flag{0};
    auto ctx_guard = boost::asio::make_work_guard(base.GetContext());
    bool driver_ok = false;
    bool delivered = false;
    int start_rc = -1;

    {
        auto ev = std::make_shared<Event>(&base, rfd, static_cast<short>(EventOpt::READABLE | EventOpt::TIMEOUT),
                                          [&](int, short flag, EventId) {
                                              seen_flag.store(flag);
                                              hits.fetch_add(1);
                                              {
                                                  std::lock_guard<std::mutex> lock(gate.m);
                                                  gate.delivered = true;
                                              }
                                              gate.cv.notify_all();
                                          });

        std::thread driver([&] {
            {
                std::lock_guard<std::mutex> lock(gate.m);
                gate.driver_ready = true;
            }
            gate.cv.notify_all();
            base.GetContext().restart();
            base.GetContext().run();
        });

        {
            std::unique_lock<std::mutex> lock(gate.m);
            driver_ok = gate.cv.wait_for(lock, std::chrono::seconds(5), [&] { return gate.driver_ready; });
        }

        if (driver_ok)
            start_rc = ev->StartListen(20);

        if (driver_ok) {
            std::unique_lock<std::mutex> lock(gate.m);
            delivered = gate.cv.wait_for(lock, std::chrono::seconds(5), [&] { return gate.delivered; });
        }

        ctx_guard.reset();
        driver.join();
    }

    Event::SetTestHook(nullptr);
    g_interleave_gate = nullptr;

    BOOST_REQUIRE_MESSAGE(driver_ok, "驱动线程未进入 run()");
    BOOST_REQUIRE_EQUAL(start_rc, 0);
    BOOST_REQUIRE_MESSAGE(delivered, "装配期间已就绪的 fd 未交付");
    {
        std::lock_guard<std::mutex> lock(gate.m);
        BOOST_CHECK_MESSAGE(gate.overlap, "完成回调没有与 StartListen 装配重叠");
    }
    BOOST_CHECK_EQUAL(hits.load(), 1); // timer 已撤销：join 后没有第二次交付
    BOOST_CHECK_EQUAL(seen_flag.load(), static_cast<short>(EventOpt::READABLE));

    ::close(rfd);
    ::close(wfd);
}

BOOST_AUTO_TEST_SUITE_END()
