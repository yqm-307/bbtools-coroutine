#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>

#include <bbt/coroutine/detail/Scheduler.hpp>
#ifdef EPOLLIN
#error "Scheduler.hpp leaked sys/epoll.h; epoll is not the EventLoop contract"
#endif

#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/pollevent/Event.hpp>

using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(EventLoopBoundaryTest)

BOOST_AUTO_TEST_CASE(t_timer)
{
    std::atomic_int n{0};
    auto co = Coroutine::Create(4096, [](){});
    auto event = CoPollEvent::Create(co->GetId(), [&](auto, int, int){ n++; });

    BOOST_REQUIRE_EQUAL(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, 40), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_REQUIRE(!event->CommitPark());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (n.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        CoPoller::GetInstance()->PollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    BOOST_CHECK_EQUAL(n.load(), 1);
    event->UnRegist();
}

BOOST_AUTO_TEST_CASE(t_fd)
{
    int fds[2];
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);

    std::atomic_int n{0};
    auto co = Coroutine::Create(4096, [](){});
    auto event = CoPollEvent::Create(co->GetId(), [&](auto, int, int){ n++; });

    BOOST_REQUIRE_EQUAL(event->InitFdEvent(fds[0], bbt::pollevent::EventOpt::READABLE, 500), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_REQUIRE(!event->CommitPark());

    char c = 'x';
    BOOST_REQUIRE_EQUAL(::write(fds[1], &c, 1), 1);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (n.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        CoPoller::GetInstance()->PollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    BOOST_CHECK_EQUAL(n.load(), 1);
    event->UnRegist();

    ::close(fds[0]);
    ::close(fds[1]);
}

BOOST_AUTO_TEST_CASE(t_wakeup)
{
    std::atomic_int n{0};
    auto event = CoPollEvent::Create(1, [&](auto, int flags, int){
        n++;
        BOOST_CHECK_EQUAL(flags, POLL_EVENT_CUSTOM);
    });

    BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_REQUIRE_EQUAL(event->CommitPark(), false);
    BOOST_REQUIRE_EQUAL(CoPoller::GetInstance()->NotifyCustomEvent(event), 0);
    BOOST_CHECK_EQUAL(n.load(), 1);
    event->UnRegist();
}

BOOST_AUTO_TEST_SUITE_END()
