#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
using namespace bbt::coroutine;

#define PrintTime(flag) printf("标记点=[%s]   协程id=[%ld] 时间戳=[%ld]\n", flag, GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());

BOOST_AUTO_TEST_SUITE(CoCondTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    g_scheduler->Start();
}

BOOST_AUTO_TEST_CASE(t_cond_multi)
{
    std::atomic_bool run_in_co{false};

    bbtco [&run_in_co](){
        auto co1 = sync::CoWaiter::Create();
        auto co2 = sync::CoWaiter::Create();
        Assert(co1 != nullptr && co2 != nullptr);

        bbtco [&run_in_co, co1, co2](){
            BOOST_ASSERT(!run_in_co.load());
            run_in_co = true;
            co1->Notify();
            co2->Wait();
            BOOST_ASSERT(!run_in_co.load());
            run_in_co = true;
            co1->Notify();
            co1->Notify();
            co2->Wait();
        };

        co1->Wait();
        BOOST_ASSERT(run_in_co.load());
        run_in_co = false;
        co2->Notify();
        co1->Wait();
        BOOST_ASSERT(run_in_co.load());
        run_in_co = false;
        co2->Notify();
    };

    // 非阻塞情况下程序最多活10s
    auto max_end_ts = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(100));

    /* 开始轮询，探测完成的事件并回调通知到协程事件完成 */
    while (!bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(max_end_ts))
    {
        std::this_thread::sleep_for(bbt::core::clock::milliseconds(10));
    }

}

BOOST_AUTO_TEST_CASE(t_cond_wait_with_timeout)
{
    const int wait_ms = 200;
    int a = 0;
    bbtco [&](){
        auto cond = sync::CoWaiter::Create();
        auto begin = bbt::core::clock::gettime();
        cond->WaitWithTimeout(wait_ms);
        auto end = bbt::core::clock::gettime();
        a++;

        BOOST_CHECK_GE(end - begin, wait_ms);
    };

    // 非阻塞情况下程序最多活1s
    auto max_end_ts = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(500));

    /* 开始轮询，探测完成的事件并回调通知到协程事件完成 */
    while (a <= 0 && !bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(max_end_ts))
        std::this_thread::sleep_for(bbt::core::clock::milliseconds(10));

    BOOST_CHECK(a == 1);
}

BOOST_AUTO_TEST_CASE(t_cond_wait)
{
    std::mutex lock;
    const int nmax_co = 1000;
    std::atomic_int ncount{0};
    bbt::core::thread::CountDownLatch l2{nmax_co};

    bbtco [&](){
        auto cond = sync::CoCond::Create();

        for (int i = 0; i < nmax_co; ++i) {
            bbtco [&](){
                cond->Wait();
                ncount++;
                l2.Down();
            };
        }

        sleep(1);
        cond->NotifyAll();
    };

    l2.Wait();
    BOOST_CHECK(ncount == nmax_co);
}

BOOST_AUTO_TEST_CASE(t_cond_notify_skips_expired_waiter)
{
    auto cond = sync::CoCond::Create();
    std::atomic_int timeout_result{-2};
    std::atomic_int wake_result{-2};
    bbt::core::thread::CountDownLatch latch{2};

    bbtco [cond, &timeout_result, &latch]() {
        timeout_result = cond->WaitFor(20);
        latch.Down();
    };

    bbtco [cond, &wake_result, &latch]() {
        wake_result = cond->Wait();
        latch.Down();
    };

    bbtco [cond]() {
        bbtco_sleep(60);
        BOOST_CHECK_EQUAL(cond->NotifyOne(), 0);
    };

    latch.Wait();

    BOOST_CHECK_EQUAL(timeout_result.load(), 1);
    BOOST_CHECK_EQUAL(wake_result.load(), 0);
}

BOOST_AUTO_TEST_CASE(t_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()