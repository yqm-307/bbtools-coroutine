#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <iostream>
#include <bbt/base/thread/Lock.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE()

BOOST_AUTO_TEST_CASE(t_begin)
{
    g_scheduler->Start(true);
}

/* 单读单写 */
BOOST_AUTO_TEST_CASE(t_chan_1_vs_1)
{
    std::atomic_int count = 0;
    bbtco [&count](){
        sync::Chan<int> c;
        
        bbtco [&c](){
            for (int i = 0; i < 100; ++i)
                BOOST_ASSERT(c.Write(i) == 0);
        };

        for (int i = 0; i < 100; ++i) {
            int val;
            BOOST_ASSERT(c.Read(val) == 0);
            count++;
        }
    };

    sleep(1);
    BOOST_CHECK_EQUAL(count.load(), 100);
}

/* 单读多写 */
BOOST_AUTO_TEST_CASE(t_chan_1_vs_n)
{
    std::atomic_int count = 0;
    bbtco [&count](){
        sync::Chan<int> c{100*1000};
        
        for (int i = 0; i < 1000; ++i)
        {
            bbtco [&c, i](){
                for (int i = 0; i < 100; ++i) {
                    int ret = c.Write(i);
                    BOOST_ASSERT(ret == 0);
                }
            };
        }

        for (int i = 0; i < 1000 * 100; ++i) {
            int val;
            BOOST_ASSERT(c.Read(val) == 0);
            count++;                                                                                                                                                                                    
        }
    };
    sleep(5);
    BOOST_CHECK_EQUAL(count.load(), 100 * 1000);
}

BOOST_AUTO_TEST_CASE(t_chan_operator_overload)
{
    std::atomic_int count = 0;
    int wait_ms = 100;
    bbt::thread::CountDownLatch l{1};

    bbtco [&count, wait_ms, &l] () {
        bool succ = false;
        auto chan = Chan<int>();
        int write_val = 1;
        succ = chan << write_val;
        BOOST_ASSERT(succ);

        int val = 0;
        succ = chan >> val;
        BOOST_ASSERT(succ);
        BOOST_ASSERT(val == 1);

        int ret = chan->TryRead(val, wait_ms);
        BOOST_ASSERT(ret != 0);
        l.Down();
    };

    l.Wait();

    l.Reset(1);

    bbtco [&count, wait_ms, &l](){
        bool succ = false;
        auto chan = sync::Chan<int>();
        int write_val = 1;
        succ = chan << write_val;
        BOOST_ASSERT(succ);

        int read_val = 0;
        succ = chan >> read_val;
        BOOST_ASSERT(succ);
        BOOST_ASSERT(read_val == 1);

        int ret = chan.TryRead(read_val, wait_ms);
        BOOST_ASSERT(ret != 0);
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_close) 
{
    std::atomic_bool flag{false};
    
    auto chan = Chan<int>();
    bbtco [&flag, &chan](){
        bbtco [&chan](){
            int block;
            chan->TryRead(block, 100); // 阻塞100ms
            chan->Close();
        };

        bbtco [&chan, &flag](){
            int val;
            chan >> val;
            flag = true;
        };
    };
    sleep(1);
    BOOST_CHECK(flag);
}

BOOST_AUTO_TEST_CASE(t_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()