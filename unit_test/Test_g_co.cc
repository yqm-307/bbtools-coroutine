#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/coroutine.hpp>
BOOST_AUTO_TEST_SUITE(CoroutineTest)

BOOST_AUTO_TEST_CASE(t_multi_coroutine)
{
    std::atomic_int ncount = 0;
    g_scheduler->Start(true);

    for (int i = 0; i < 100000; ++i)
    {
        bbtco [&](){
            ncount++;
        };
    }

    std::this_thread::sleep_for(bbt::clock::milliseconds(1000));
    g_scheduler->Stop();

    BOOST_CHECK_EQUAL(ncount, 100000);
}

BOOST_AUTO_TEST_SUITE_END()