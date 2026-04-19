#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <thread>
#include <vector>
#include <bbt/coroutine/detail/WaitProtocol.hpp>

using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(WaitProtocolStressTest)

BOOST_AUTO_TEST_CASE(t_wait_protocol_high_iteration_race)
{
    constexpr int iterations = 2000;

    for (int i = 0; i < iterations; ++i)
    {
        WaitNode node;
        std::atomic_int winners{0};
        std::vector<std::thread> workers;

        BOOST_REQUIRE(node.Arm());
        workers.emplace_back([&]() {
            if (node.CompleteReady())
                winners.fetch_add(1);
        });
        workers.emplace_back([&]() {
            if (node.CompleteTimeout())
                winners.fetch_add(1);
        });
        workers.emplace_back([&]() {
            if (node.CompleteCanceled())
                winners.fetch_add(1);
        });

        for (auto& worker : workers)
            worker.join();

        BOOST_CHECK_EQUAL(winners.load(), 1);
        BOOST_CHECK(node.IsTerminal());
        BOOST_CHECK(node.GetResult() != WaitResult::WAIT_NONE);
    }
}

BOOST_AUTO_TEST_SUITE_END()