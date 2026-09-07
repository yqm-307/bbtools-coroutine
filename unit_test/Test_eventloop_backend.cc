#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <bbt/coroutine/detail/Scheduler.hpp>
#if defined(BOOST_ASIO_HPP) || defined(ASIO_HPP)
#error "Scheduler.hpp leaked ASIO EventLoop backend"
#endif

#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/pollevent/Event.hpp>

using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(EventLoopBackendTest)

BOOST_AUTO_TEST_CASE(t_backend_is_eventloop)
{
    auto loop = CoPoller::GetInstance()->GetEventLoop();
    BOOST_REQUIRE(loop != nullptr);

    std::atomic_int n{0};
    auto event = CoPollEvent::Create(1, [&](auto, int, int){ n++; });

    BOOST_REQUIRE_EQUAL(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, 40), 0);
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_REQUIRE_EQUAL(event->CommitPark(), false);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (n.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        BOOST_REQUIRE(CoPoller::GetInstance()->GetEventLoop() == loop);
        CoPoller::GetInstance()->PollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    BOOST_CHECK_EQUAL(n.load(), 1);
    event->UnRegist();
}

BOOST_AUTO_TEST_SUITE_END()
