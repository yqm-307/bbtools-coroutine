#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
using namespace bbt::coroutine;

#define PrintTime(flag) printf("标记点=[%s]   协程id=[%ld] 时间戳=[%ld]\n", flag, GetLocalCoroutineId(), bbt::clock::now<>().time_since_epoch().count());

BOOST_AUTO_TEST_SUITE(CoCondTest)

BOOST_AUTO_TEST_CASE(t_cond)
{
    const int n_max_notify_count = 500;
    std::atomic_int ncount = 0;
    uint64_t n_begin_time = bbt::clock::now<>().time_since_epoch().count();
    uint64_t n_last_time = 0;
    g_scheduler->Start(true);

    bbtco [&]() {
        auto cond = sync::CoCond::Create();
        Assert(cond != nullptr);
        for (int i = 0; i < n_max_notify_count; ++i) {
            g_scheduler->RegistCoroutineTask([i, cond, &n_last_time, &ncount](){
                cond->Notify();
                printf("[%d] now: %ld diff: %ld\n", i, bbt::clock::now<>().time_since_epoch().count(), bbt::clock::now<>().time_since_epoch().count() - n_last_time);
                n_last_time = bbt::clock::now<>().time_since_epoch().count();
                ncount++;
            });
            cond->Wait();
        }
    };

    // 非阻塞情况下程序最多活10s
    auto max_end_ts = bbt::clock::nowAfter(bbt::clock::seconds(10));

    /* 开始轮询，探测完成的事件并回调通知到协程事件完成 */
    while (ncount.load() != n_max_notify_count && !bbt::clock::is_expired<bbt::clock::milliseconds>(max_end_ts))
    {
        std::this_thread::sleep_for(bbt::clock::milliseconds(10));
    }

    printf("begin time: %ld\n", n_begin_time);
    printf("end   time: %ld\n", n_last_time);
    BOOST_CHECK_EQUAL(ncount.load(), n_max_notify_count);

    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()