#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE()

BOOST_AUTO_TEST_CASE(t_begin)
{
    g_scheduler->Start();
}

BOOST_AUTO_TEST_CASE(t_test_copool)
{
    auto pool = bbtco_make_copool(100);
    const int max_co = 100000;
    bbt::thread::CountDownLatch l{max_co};
    std::atomic_int count = 0;

    for (int i = 0; i < max_co; ++i) {
        pool->Submit([&](){
            BOOST_ASSERT(GetLocalCoroutineId() > 0);
            l.Down();
            count++;
        });
    }

    l.Wait();
    pool->Release();

    BOOST_CHECK_EQUAL(count, max_co);
}

BOOST_AUTO_TEST_CASE(t_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
