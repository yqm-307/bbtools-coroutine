#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <iostream>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE()

BOOST_AUTO_TEST_CASE(t_begin)
{
    g_scheduler->Start();
}

BOOST_AUTO_TEST_CASE(t_chan_block)
{
    BOOST_TEST_MESSAGE("enter t_chan_block");
    bbtco [](){
        auto c = Chan<int, 1>();
        bbtco [&c](){ c << 12; };

        int val;
        c >> val;
        BOOST_CHECK_EQUAL(val, 12);

        bbtco [&c](){ c << 14; };
        c >> val;
        BOOST_CHECK_EQUAL(val, 14);

            bbtco [&c](){ c << 42; };
        c >> val;
        BOOST_CHECK_EQUAL(val, 42);
    };
}

/* 单读单写 */
BOOST_AUTO_TEST_CASE(t_chan_1_vs_1)
{
    BOOST_TEST_MESSAGE("enter t_chan_1_vs_1");
    std::atomic_int count = 0;
    bbt::core::thread::CountDownLatch l{1};
    bbtco [&count, &l](){
        auto c = Chan<int, 65535>();
        
        bbtco [&c](){
            for (int i = 0; i < 100; ++i)
                BOOST_ASSERT(c->Write(i) == 0);
        };

        for (int i = 0; i < 100; ++i) {
            int val;
            BOOST_ASSERT(c->Read(val) == 0);
            count++;
        }

        l.Down();
    };

    // sleep(1);
    l.Wait();
    BOOST_CHECK_EQUAL(count.load(), 100);
}

/* 单读多写 */
BOOST_AUTO_TEST_CASE(t_chan_1_vs_n)
{
    BOOST_TEST_MESSAGE("enter t_chan_1_vs_n");
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_int count = 0;
    bbtco [&](){
        auto c = Chan<int, 65535>();
        
        for (int i = 0; i < 1000; ++i)
        {
            bbtco [&c, i](){
                for (int i = 0; i < 100; ++i) {
                    int ret = c->Write(i);
                    BOOST_ASSERT(ret == 0);
                }
            };
        }

        for (int i = 0; i < 1000 * 100; ++i) {
            int val;
            BOOST_ASSERT(c->Read(val) == 0);
            count++;                                                                                                                                                                                    
        }

        l.Down();
    };

    l.Wait();
    BOOST_CHECK_EQUAL(count.load(), 100 * 1000);
}

/* 运算符重载测试 */
BOOST_AUTO_TEST_CASE(t_chan_operator_overload)
{
    BOOST_TEST_MESSAGE("enter t_chan_operator_overload");
    std::atomic_int count = 0;
    int wait_ms = 100;
    bbt::core::thread::CountDownLatch l{1};

    bbtco [&count, wait_ms, &l] () {
        bool succ = false;
        auto chan = Chan<int, 65535>();
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
        auto chan = sync::Chan<int, 65535>();
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

BOOST_AUTO_TEST_CASE(t_block_write)
{
    BOOST_TEST_MESSAGE("enter t_block_write");
    bbt::core::thread::CountDownLatch l{1};
    bbtco [&](){
        auto chan = Chan<int, 100>();
        bbtco [&](){
            int val;
            chan >> val;
            l.Down();
        };

        sleep(1);
        chan << 1;
    };

    l.Wait();
    BOOST_ASSERT(true);
}

BOOST_AUTO_TEST_CASE(t_block_write_n)
{
    BOOST_TEST_MESSAGE("enter t_block_write_n");
    bbt::core::thread::CountDownLatch l{1};
    const int nwrite = 100;
    std::atomic_int n_read_succ_num{0};
    std::set<int> results;

    bbtco [&](){
        auto chan = Chan<int, 1>();

        for (int i = 0; i < nwrite; ++i) {
            bbtco [&chan, &n_read_succ_num, i](){
                chan << i;
                n_read_succ_num++;
            };
        }

        bbt::coroutine::detail::Hook_Sleep(100);

        int val;
        for (int i = 0; i < nwrite; ++i) {
            while (!(chan >> val));
                // detail::Hook_Sleep(1);
            results.insert(val);
        }

        l.Down();
    };

    l.Wait();

    BOOST_CHECK_EQUAL(nwrite, n_read_succ_num.load());
    for (int i = 0; i < nwrite; ++i) {
        auto it = results.find(i);

        BOOST_ASSERT(it != results.end());
    }
}

/* 无缓冲单次读写 */
BOOST_AUTO_TEST_CASE(t_nocache_chan_1v1)
{
    BOOST_TEST_MESSAGE("enter t_nocache_chan_1v1");
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_bool flag{false};

    bbtco [&](){
        auto chan = Chan<int, 0>();
        auto cond = sync::CoWaiter::Create();
        bbtco [&](){
            detail::Hook_Sleep(100);
            cond->Notify();
            chan << 1;
            flag.exchange(true);
        };

        cond->Wait();
        BOOST_ASSERT(!flag);
        l.Down();
    };

    l.Wait();
}

/* 无缓冲单读多写 */
BOOST_AUTO_TEST_CASE(t_nocache_chan_1vn)
{
    BOOST_TEST_MESSAGE("enter t_nocache_chan_1vn");

    const int nwrite_co_num = 100;
    int ncount = 0;
    bbt::core::thread::CountDownLatch l{nwrite_co_num + 1};

    bbtco [&](){
        auto chan = Chan<int, 0>();
        for (int i = 0; i < nwrite_co_num; ++i)
            bbtco [&](){
                chan << 1;
                l.Down();
            };


        sleep(1);
        int val;
        while (chan >> val) {
            ncount++;
            if (ncount >= nwrite_co_num) 
                chan->Close();
        }
        l.Down();
    };

    l.Wait();

    BOOST_ASSERT(ncount == nwrite_co_num);
}

BOOST_AUTO_TEST_CASE(t_close) 
{
    BOOST_TEST_MESSAGE("enter t_close");
    std::atomic_bool flag{false};
    bbt::core::thread::CountDownLatch l{2};
    
    auto chan = Chan<int, 65535>();
    bbtco [&flag, &chan, &l](){
        bbtco [&chan, &l](){
            int block;
            chan->TryRead(block, 100); // 阻塞100ms
            chan->Close();
            l.Down();
        };

        bbtco [&chan, &flag, &l](){
            int val;
            chan >> val;
            flag = true;
            l.Down();
        };
    };

    l.Wait();
    BOOST_CHECK(flag);
}

BOOST_AUTO_TEST_CASE(t_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()