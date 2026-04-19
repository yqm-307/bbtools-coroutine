#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/WaitProtocol.hpp>

using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(WaitProtocolStateTest)

BOOST_AUTO_TEST_CASE(t_wait_node_pending_to_completed)
{
    WaitNode node;

    BOOST_CHECK(node.Arm());
    BOOST_CHECK(node.IsPending());
    BOOST_CHECK_EQUAL(node.GetResult(), WaitResult::WAIT_NONE);
    BOOST_CHECK(node.CompleteReady());
    BOOST_CHECK(node.IsTerminal());
    BOOST_CHECK_EQUAL(node.GetState(), WaitState::WAIT_COMPLETED);
    BOOST_CHECK_EQUAL(node.GetResult(), WaitResult::WAIT_READY);
}

BOOST_AUTO_TEST_CASE(t_wait_node_terminal_results_are_distinct)
{
    WaitNode timeout_node;
    WaitNode closed_node;
    WaitNode canceled_node;

    BOOST_REQUIRE(timeout_node.Arm());
    BOOST_REQUIRE(closed_node.Arm());
    BOOST_REQUIRE(canceled_node.Arm());

    BOOST_CHECK(timeout_node.CompleteTimeout());
    BOOST_CHECK(closed_node.CompleteClosed());
    BOOST_CHECK(canceled_node.CompleteCanceled());

    BOOST_CHECK_EQUAL(timeout_node.GetState(), WaitState::WAIT_TIMED_OUT);
    BOOST_CHECK_EQUAL(timeout_node.GetResult(), WaitResult::WAIT_TIMEOUT);
    BOOST_CHECK_EQUAL(closed_node.GetState(), WaitState::WAIT_CLOSED);
    BOOST_CHECK_EQUAL(closed_node.GetResult(), WaitResult::WAIT_CLOSED);
    BOOST_CHECK_EQUAL(canceled_node.GetState(), WaitState::WAIT_CANCELED);
    BOOST_CHECK_EQUAL(canceled_node.GetResult(), WaitResult::WAIT_CANCELED);
}

BOOST_AUTO_TEST_CASE(t_wait_node_cancel_fixture_tracks_canceled_state)
{
    auto coroutine = Coroutine::Create(4096, [](){});
    auto event = CoPollEvent::Create(coroutine->GetId(), [](auto, int, int){});
    WaitNode node;

    BOOST_REQUIRE(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, 1000) == 0);
    BOOST_REQUIRE(event->Regist() == 0);
    BOOST_REQUIRE(node.Arm());

    BOOST_CHECK_EQUAL(event->UnRegist(), 0);
    BOOST_CHECK(node.CompleteCanceled());
    BOOST_CHECK_EQUAL(node.GetState(), WaitState::WAIT_CANCELED);
    BOOST_CHECK_EQUAL(node.GetResult(), WaitResult::WAIT_CANCELED);
}

BOOST_AUTO_TEST_SUITE_END()