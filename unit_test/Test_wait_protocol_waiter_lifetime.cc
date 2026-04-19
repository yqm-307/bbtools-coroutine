#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <vector>
#include <bbt/coroutine/detail/WaitProtocol.hpp>

using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(WaitProtocolWaiterLifetimeTest)

BOOST_AUTO_TEST_CASE(t_stale_waiter_is_skipped_after_timeout)
{
    WaitNode node;

    BOOST_REQUIRE(node.Arm());
    BOOST_CHECK(node.CompleteTimeout());
    BOOST_CHECK(!node.CompleteReady());
    BOOST_CHECK_EQUAL(node.GetState(), WaitState::WAIT_TIMED_OUT);
    BOOST_CHECK_EQUAL(node.GetResult(), WaitResult::WAIT_TIMEOUT);
}

BOOST_AUTO_TEST_CASE(t_destroy_flushes_pending_waiters)
{
    std::vector<WaitNode> waiters(8);

    for (auto& waiter : waiters)
        BOOST_REQUIRE(waiter.Arm());

    for (auto& waiter : waiters)
        BOOST_CHECK(waiter.CompleteClosed());

    for (auto& waiter : waiters)
    {
        BOOST_CHECK(waiter.IsTerminal());
        BOOST_CHECK_EQUAL(waiter.GetState(), WaitState::WAIT_CLOSED);
        BOOST_CHECK_EQUAL(waiter.GetResult(), WaitResult::WAIT_CLOSED);
    }
}

BOOST_AUTO_TEST_CASE(t_repeated_notify_does_not_touch_completed_waiter)
{
    WaitNode node;

    BOOST_REQUIRE(node.Arm());
    BOOST_CHECK(node.CompleteReady());
    BOOST_CHECK(!node.CompleteReady());
    BOOST_CHECK(!node.CompleteCanceled());
    BOOST_CHECK_EQUAL(node.GetState(), WaitState::WAIT_COMPLETED);
    BOOST_CHECK_EQUAL(node.GetResult(), WaitResult::WAIT_READY);
}

BOOST_AUTO_TEST_SUITE_END()