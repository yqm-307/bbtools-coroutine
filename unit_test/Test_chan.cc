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

/* 单读单写 */
BOOST_AUTO_TEST_CASE(t_chan_1_vs_1)
{
    g_scheduler->Start(true);
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
    g_scheduler->Stop();
}

/* 单读多写 */
BOOST_AUTO_TEST_CASE(t_chan_1_vs_n)
{
    g_scheduler->Start(true);
    /* 跑100轮 */
    // for (int i = 0; i < 10; ++i)
    // {
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
    // }

    g_scheduler->Stop();
}

BOOST_AUTO_TEST_CASE(t_chan_operator_overload)
{
    g_scheduler->Start(true);

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

        auto begin = bbt::clock::gettime();
        succ = chan->TryRead(val, wait_ms);
        auto end = bbt::clock::gettime();
        BOOST_ASSERT(!succ);
        BOOST_ASSERT(end - begin >= wait_ms);
        l.Down();
    };

    l.Wait();

    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()