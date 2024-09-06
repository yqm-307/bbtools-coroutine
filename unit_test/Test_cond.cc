#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
using namespace bbt::coroutine;

#define PrintTime(flag) printf("标记点=[%s]   协程id=[%ld] 时间戳=[%ld]\n", flag, GetLocalCoroutineId(), bbt::clock::now<>().time_since_epoch().count());

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
    auto max_end_ts = bbt::clock::nowAfter(bbt::clock::milliseconds(100));

    /* 开始轮询，探测完成的事件并回调通知到协程事件完成 */
    while (!bbt::clock::is_expired<bbt::clock::milliseconds>(max_end_ts))
    {
        std::this_thread::sleep_for(bbt::clock::milliseconds(10));
    }

}

BOOST_AUTO_TEST_CASE(t_cond_wait_with_timeout)
{
    const int wait_ms = 200;
    int a = 0;
    bbtco [&](){
        auto cond = sync::CoWaiter::Create();
        auto begin = bbt::clock::gettime();
        cond->WaitWithTimeout(wait_ms);
        auto end = bbt::clock::gettime();
        a++;

        BOOST_CHECK_GE(end - begin, wait_ms);
    };

    // 非阻塞情况下程序最多活1s
    auto max_end_ts = bbt::clock::nowAfter(bbt::clock::milliseconds(500));

    /* 开始轮询，探测完成的事件并回调通知到协程事件完成 */
    while (a <= 0 && !bbt::clock::is_expired<bbt::clock::milliseconds>(max_end_ts))
        std::this_thread::sleep_for(bbt::clock::milliseconds(10));

    BOOST_CHECK(a == 1);
}

BOOST_AUTO_TEST_CASE(t_cond_wait)
{
    std::mutex lock;
    const int nmax_co = 1000;
    std::atomic_int ncount{0};
    bbt::thread::CountDownLatch l2{nmax_co};

    bbtco [&](){
        auto cond = sync::CoCond::Create(lock);

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

BOOST_AUTO_TEST_CASE(t_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()