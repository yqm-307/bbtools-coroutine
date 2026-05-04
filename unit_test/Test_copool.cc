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
    bbt::core::thread::CountDownLatch l{max_co};
    std::atomic_int count = 0;

    for (int i = 0; i < max_co; ++i) {
        pool->Submit([&](){
            BOOST_ASSERT(GetLocalCoroutineId() > 0);
            count++;
            l.Down();
        });
    }

    l.Wait();

    BOOST_CHECK_EQUAL(count, max_co);
}

BOOST_AUTO_TEST_CASE(t_submit_overloads_wake_workers)
{
    auto pool = bbtco_make_copool(1);
    bbt::core::thread::CountDownLatch l{2};

    auto lvalue_work = [&]() {
        l.Down();
    };

    BOOST_REQUIRE_EQUAL(pool->Submit(lvalue_work), 0);
    BOOST_REQUIRE_EQUAL(pool->Submit([&]() {
        l.Down();
    }), 0);

    l.Wait();
    pool->Release();
}

BOOST_AUTO_TEST_CASE(t_release_drains_queued_submit_overloads)
{
    auto pool = bbtco_make_copool(1);
    std::atomic_int count = 0;
    bbt::core::thread::CountDownLatch worker_started{1};
    bbt::core::thread::CountDownLatch release_blocker{1};

    auto first_work = [&](){
        worker_started.Down();
        release_blocker.Wait();
        ++count;
    };

    BOOST_REQUIRE_EQUAL(pool->Submit(first_work), 0);
    worker_started.Wait();

    auto queued_lvalue = [&](){
        ++count;
    };

    BOOST_REQUIRE_EQUAL(pool->Submit(queued_lvalue), 0);
    BOOST_REQUIRE_EQUAL(pool->Submit([&](){
        ++count;
    }), 0);

    release_blocker.Down();
    pool->Release();

    BOOST_CHECK_EQUAL(count.load(), 3);
}

BOOST_AUTO_TEST_CASE(t_release_stops_new_submit)
{
    auto pool = bbtco_make_copool(1);
    pool->Release();

    auto lvalue_work = []() {};
    BOOST_CHECK_EQUAL(pool->Submit(lvalue_work), -1);
    BOOST_CHECK_EQUAL(pool->Submit([]() {}), -1);
}

BOOST_AUTO_TEST_CASE(t_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
