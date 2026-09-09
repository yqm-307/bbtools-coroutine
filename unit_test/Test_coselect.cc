#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <vector>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/sync/CoSelect.hpp>
using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE(CoSelectTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    g_scheduler->Start();
}

// 两路 CaseRead：只往 ch1 写，Run 返回 0，out 正确；且必须被写入唤醒而非超时兜底
BOOST_AUTO_TEST_CASE(t_select_case_read)
{
    bbt::core::thread::CountDownLatch l{1};
    int index = -999;
    int out1 = 0;
    int out2 = 0;
    int elapsed_ms = -1;

    bbtco [&](){
        sync::Chan<int, 4> ch1;
        sync::Chan<int, 4> ch2;

        bbtco [&ch1](){
            bbtco_sleep(30);    // 等 select 挂起后再写，验证 watcher 唤醒路径
            BOOST_CHECK_EQUAL(ch1.TryWrite(42), 0);
        };

        auto begin = bbt::core::clock::gettime_mono();
        index = sync::CoSelect()
                    .CaseRead(ch1, out1)
                    .CaseRead(ch2, out2)
                    .CaseTimeout(2000)
                    .Run();
        elapsed_ms = bbt::core::clock::gettime_mono() - begin;
        l.Down();
    };

    l.Wait();
    BOOST_CHECK_EQUAL(index, 0);
    BOOST_CHECK_EQUAL(out1, 42);
    // 写入发生在 30ms：唤醒必须及时，不能靠 2000ms 超时后的重试兜底（#314）
    BOOST_CHECK_LT(elapsed_ms, 500);
}

// 两路都有数据：命中注册序优先（0 或 1），对应 out 正确
BOOST_AUTO_TEST_CASE(t_select_both_ready_first_registered)
{
    bbt::core::thread::CountDownLatch l{1};
    int index = -999;
    int out1 = 0;
    int out2 = 0;

    bbtco [&](){
        sync::Chan<int, 4> ch1;
        sync::Chan<int, 4> ch2;
        BOOST_CHECK_EQUAL(ch1.TryWrite(11), 0);
        BOOST_CHECK_EQUAL(ch2.TryWrite(22), 0);

        index = sync::CoSelect()
                    .CaseRead(ch1, out1)
                    .CaseRead(ch2, out2)
                    .Run();
        l.Down();
    };

    l.Wait();
    // 注册序优先，且未挂起即命中
    BOOST_CHECK_EQUAL(index, 0);
    BOOST_CHECK_EQUAL(out1, 11);
}

// Default + 空 chan：立即 -1，不挂起
BOOST_AUTO_TEST_CASE(t_select_default)
{
    bbt::core::thread::CountDownLatch l{1};
    int index = -999;
    int elapsed = -1;

    bbtco [&](){
        sync::Chan<int, 4> ch;
        int out = 0;
        auto begin = bbt::core::clock::gettime_mono();
        index = sync::CoSelect()
                    .CaseRead(ch, out)
                    .Default()
                    .Run();
        elapsed = bbt::core::clock::gettime_mono() - begin;
        l.Down();
    };

    l.Wait();
    BOOST_CHECK_EQUAL(index, -1);
    BOOST_CHECK_LT(elapsed, 50);
}

// CaseTimeout(50) + 空 chan：返回 -1，耗时 >=40ms 且 <1000ms
BOOST_AUTO_TEST_CASE(t_select_timeout)
{
    bbt::core::thread::CountDownLatch l{1};
    int index = -999;
    int elapsed = -1;

    bbtco [&](){
        sync::Chan<int, 4> ch;
        int out = 0;
        auto begin = bbt::core::clock::gettime_mono();
        index = sync::CoSelect()
                    .CaseRead(ch, out)
                    .CaseTimeout(50)
                    .Run();
        elapsed = bbt::core::clock::gettime_mono() - begin;
        l.Down();
    };

    l.Wait();
    BOOST_CHECK_EQUAL(index, -1);
    BOOST_CHECK_GE(elapsed, 40);
    BOOST_CHECK_LT(elapsed, 1000);
}

// CaseWrite：满缓冲的 chan 等读端腾出后写成功
BOOST_AUTO_TEST_CASE(t_select_case_write)
{
    bbt::core::thread::CountDownLatch l{1};
    int index = -999;
    int written = 0;

    bbtco [&](){
        sync::Chan<int, 1> ch;
        BOOST_CHECK_EQUAL(ch.TryWrite(99), 0);   // 填满

        bbtco [&ch](){
            bbtco_sleep(30);                    // 等 select 挂起后腾出缓冲
            int v = 0;
            BOOST_CHECK_EQUAL(ch.Read(v), 0);
            BOOST_CHECK_EQUAL(v, 99);
        };

        index = sync::CoSelect()
                    .CaseWrite(ch, 7)
                    .CaseTimeout(2000)
                    .Run();

        // 写命中后 7 应在队列里
        int out = 0;
        written = ch.TryRead(out);
        BOOST_CHECK_EQUAL(out, 7);
        l.Down();
    };

    l.Wait();
    BOOST_CHECK_EQUAL(index, 0);
    BOOST_CHECK_EQUAL(written, 0);
}

// 挂起期间 ticker 协程仍在跑（调度没有卡死）
BOOST_AUTO_TEST_CASE(t_select_ticker_alive_while_waiting)
{
    bbt::core::thread::CountDownLatch l{1};
    int index = -999;
    std::atomic_int tick_count{0};

    bbtco [&](){
        std::atomic_bool ticker_done{false};
        bbtco [&tick_count, &ticker_done](){
            for (int i = 0; i < 30; ++i) {
                bbtco_sleep(10);
                ++tick_count;
            }
            ticker_done = true;
        };

        sync::Chan<int, 4> ch;
        int out = 0;
        index = sync::CoSelect()
                    .CaseRead(ch, out)
                    .CaseTimeout(200)
                    .Run();

        // 等 ticker 收尾，避免栈上引用悬垂
        while (!ticker_done)
            bbtco_sleep(10);
        l.Down();
    };

    l.Wait();
    BOOST_CHECK_EQUAL(index, -1);            // 无数据 → 超时
    BOOST_CHECK_GT(tick_count.load(), 0);
}

// Close 唤醒：select 等空 chan（无超时），另一协程 Close，Run 快速返回
BOOST_AUTO_TEST_CASE(t_select_close_wakeup)
{
    bbt::core::thread::CountDownLatch l{1};
    int index = -999;
    int elapsed = -1;

    bbtco [&](){
        sync::Chan<int, 4> ch;
        int out = 0;

        bbtco [&ch](){
            bbtco_sleep(30);
            ch.Close();
        };

        auto begin = bbt::core::clock::gettime_mono();
        // 故意不给 CaseTimeout：只能被 Close 唤醒
        index = sync::CoSelect()
                    .CaseRead(ch, out)
                    .Run();
        elapsed = bbt::core::clock::gettime_mono() - begin;
        l.Down();
    };

    l.Wait();
    BOOST_CHECK_EQUAL(index, 0);             // closed 视为就绪
    BOOST_CHECK_LT(elapsed, 1000);
}

BOOST_AUTO_TEST_CASE(t_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
