#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE()

BOOST_AUTO_TEST_CASE(t_coroutine)
{
    std::atomic_uint64_t ncount = 0;
    g_scheduler->Start(true);

    while (true)
    {
        for (int i = 0; i < 500; ++i)
        {
            g_scheduler->RegistCoroutineTask([&](){
                ncount++;
            });
        }

        std::this_thread::sleep_for(bbt::clock::seconds(1));
        printf("1 sec : %ld\n", ncount.load());
    }

    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()