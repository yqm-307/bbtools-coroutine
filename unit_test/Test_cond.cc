#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
using namespace bbt::coroutine;

#define PrintTime(flag) printf("标记点=[%s]   协程id=[%ld] 时间戳=[%ld]\n", flag, GetLocalCoroutineId(), bbt::clock::now<>().time_since_epoch().count());

BOOST_AUTO_TEST_SUITE(CoCondTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    g_scheduler->Start(true);
}

BOOST_AUTO_TEST_CASE(t_cond_multi)
{
    static std::atomic_int val;
    bbtco [](){
        auto co1 = sync::CoCond::Create();
        auto co2 = sync::CoCond::Create();
        Assert(co1 != nullptr && co2 != nullptr);

        bbtco [co1, co2](){
            co1->Notify();
            co2->Wait();
            co1->Notify();
            co1->Notify();
            co2->Wait();
            printf("co2 end\n");
        };

        co1->Wait();
        co2->Notify();
        co1->Wait();
        co2->Notify();
        printf("co1 end\n");
    };

    // 非阻塞情况下程序最多活10s
    auto max_end_ts = bbt::clock::nowAfter(bbt::clock::seconds(1));

    /* 开始轮询，探测完成的事件并回调通知到协程事件完成 */
    while (!bbt::clock::is_expired<bbt::clock::milliseconds>(max_end_ts))
    {
        std::this_thread::sleep_for(bbt::clock::milliseconds(10));
    }

}

BOOST_AUTO_TEST_CASE(t_cond_wait_with_timeout)
{
    const int wait_ms = 200;
    uint64_t begin = 0;
    uint64_t end = 0;
    bbtco [&](){
        auto cond = sync::CoCond::Create();
        begin = bbt::clock::gettime();
        cond->WaitWithTimeout(wait_ms);
        end = bbt::clock::gettime();
    };

    // 非阻塞情况下程序最多活1s
    auto max_end_ts = bbt::clock::nowAfter(bbt::clock::seconds(1));

    /* 开始轮询，探测完成的事件并回调通知到协程事件完成 */
    while (!bbt::clock::is_expired<bbt::clock::milliseconds>(max_end_ts))
    {
        std::this_thread::sleep_for(bbt::clock::milliseconds(10));
    }
    BOOST_CHECK_MESSAGE((end - begin) >= wait_ms, "begin=" << begin << "\tend=" << end << "\twait=" << wait_ms);
}

BOOST_AUTO_TEST_CASE(t_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()