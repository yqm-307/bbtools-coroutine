#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <iostream>
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
        sync::Chan c;
        
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
    for (int i = 0; i < 10; ++i)
    {
        std::atomic_int count = 0;
        bbtco [&count](){
            sync::Chan c{100*1000};
            
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
        sleep(1);
        BOOST_CHECK_EQUAL(count.load(), 100 * 1000);
    }

    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()