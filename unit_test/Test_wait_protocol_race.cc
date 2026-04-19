#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <thread>
#include <bbt/coroutine/detail/WaitProtocol.hpp>

using namespace bbt::coroutine::detail;

namespace
{

template <typename Left, typename Right>
std::pair<int, WaitResult> RunRace(Left left, Right right)
{
    WaitNode node;
    std::atomic_int winners{0};

    BOOST_REQUIRE(node.Arm());

    std::thread lhs([&]() {
        if (left(node))
            winners.fetch_add(1);
    });

    std::thread rhs([&]() {
        if (right(node))
            winners.fetch_add(1);
    });

    lhs.join();
    rhs.join();
    return {winners.load(), node.GetResult()};
}

}

BOOST_AUTO_TEST_SUITE(WaitProtocolRaceTest)

BOOST_AUTO_TEST_CASE(t_notify_vs_timeout_complete_once)
{
    auto [winners, result] = RunRace(
        [](WaitNode& node) { return node.CompleteReady(); },
        [](WaitNode& node) { return node.CompleteTimeout(); });

    BOOST_CHECK_EQUAL(winners, 1);
    BOOST_CHECK(result == WaitResult::WAIT_READY || result == WaitResult::WAIT_TIMEOUT);
}

BOOST_AUTO_TEST_CASE(t_close_vs_timeout_complete_once)
{
    auto [winners, result] = RunRace(
        [](WaitNode& node) { return node.CompleteClosed(); },
        [](WaitNode& node) { return node.CompleteTimeout(); });

    BOOST_CHECK_EQUAL(winners, 1);
    BOOST_CHECK(result == WaitResult::WAIT_CLOSED || result == WaitResult::WAIT_TIMEOUT);
}

BOOST_AUTO_TEST_CASE(t_cancel_vs_notify_complete_once)
{
    auto [winners, result] = RunRace(
        [](WaitNode& node) { return node.CompleteCanceled(); },
        [](WaitNode& node) { return node.CompleteReady(); });

    BOOST_CHECK_EQUAL(winners, 1);
    BOOST_CHECK(result == WaitResult::WAIT_CANCELED || result == WaitResult::WAIT_READY);
}

BOOST_AUTO_TEST_SUITE_END()