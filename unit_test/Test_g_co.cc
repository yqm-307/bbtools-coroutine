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
    const int nco = 100000;
    g_scheduler->Start();

    for (int i = 0; i < nco; ++i)
    {
        bool succ = false;
        while (!succ)
        {
            bbtco_noexcept(&succ) [&](){
                ncount++;
            };
        }
    }

    std::this_thread::sleep_for(bbt::core::clock::milliseconds(1000));
    g_scheduler->Stop();

    BOOST_CHECK_EQUAL(ncount, nco);
}

BOOST_AUTO_TEST_SUITE_END()