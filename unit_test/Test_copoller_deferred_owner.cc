#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <bbt/pollevent/Event.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>

using namespace bbt::coroutine::detail;

namespace
{

/* 尚未 StartListen 的 timer Event：仅用于验证析构所有权，无副作用。 */
std::shared_ptr<bbt::pollevent::Event> MakeUnarmedTimerEvent(CoPoller& poller)
{
    return poller.CreateEvent(-1, bbt::pollevent::EventOpt::TIMEOUT,
                              [](int, short, int64_t) {});
}

}

BOOST_AUTO_TEST_SUITE(CoPollerDeferredOwnerTest)

/* 非驱动线程调用 FlushDeferredEvents 是 no-op：Event 不析构，仅计数。 */
BOOST_AUTO_TEST_CASE(t_off_driver_flush_is_noop_and_counted)
{
    CoPoller poller;
    auto event = MakeUnarmedTimerEvent(poller);
    std::weak_ptr<bbt::pollevent::Event> weak = event;
    poller.DeferDestroyEvent(std::move(event));
    BOOST_REQUIRE(!weak.expired());

    std::thread other([&] { poller.FlushDeferredEvents(); });
    other.join();

    BOOST_CHECK(!weak.expired());
    BOOST_CHECK(poller.GetDrainOffDriverCount() > 0);
    BOOST_CHECK_EQUAL(poller.GetDrainInPollCount(), 0u);
}

/* 驱动线程首次 PollOnce 前兑现积压：非驱动线程的 no-op 调用不丢事件。 */
BOOST_AUTO_TEST_CASE(t_poll_once_drains_backlog_on_driver)
{
    CoPoller poller;
    auto event = MakeUnarmedTimerEvent(poller);
    std::weak_ptr<bbt::pollevent::Event> weak = event;
    poller.DeferDestroyEvent(std::move(event));

    std::thread other([&] { poller.FlushDeferredEvents(); });
    other.join();
    BOOST_REQUIRE(!weak.expired());

    poller.PollOnce();
    BOOST_CHECK(weak.expired());
}

/* 驱动线程非 poll 内调用：正常兑现。 */
BOOST_AUTO_TEST_CASE(t_driver_flush_outside_poll_destroys_event)
{
    CoPoller poller;
    poller.PollOnce();  // 本线程成为驱动线程

    auto event = MakeUnarmedTimerEvent(poller);
    std::weak_ptr<bbt::pollevent::Event> weak = event;
    poller.DeferDestroyEvent(std::move(event));

    poller.FlushDeferredEvents();

    BOOST_CHECK(weak.expired());
    BOOST_CHECK_EQUAL(poller.GetDrainOffDriverCount(), 0u);
    BOOST_CHECK_EQUAL(poller.GetDrainInPollCount(), 0u);
}

/* handler 内（poll 中）调用 FlushDeferredEvents 是 no-op：仅计数。 */
BOOST_AUTO_TEST_CASE(t_in_poll_flush_is_noop_and_counted)
{
    CoPoller poller;
    std::atomic_int callbacks{0};
    bool flushed_in_poll = false;

    auto timer = poller.CreateEvent(-1, bbt::pollevent::EventOpt::TIMEOUT,
        [&](int, short, int64_t) {
            callbacks.fetch_add(1);
            poller.FlushDeferredEvents();  // 此刻 m_in_poll == true
            flushed_in_poll = true;
        });
    BOOST_REQUIRE_EQUAL(timer->StartListen(1), 0);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!flushed_in_poll && std::chrono::steady_clock::now() < deadline)
    {
        poller.PollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    BOOST_REQUIRE(flushed_in_poll);
    BOOST_CHECK_EQUAL(callbacks.load(), 1);
    BOOST_CHECK(poller.GetDrainInPollCount() > 0);
    BOOST_CHECK_EQUAL(poller.GetDrainOffDriverCount(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
