#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

int main()
{
    std::atomic_uint64_t nocache_chan_read_count{0};
    std::atomic_uint64_t nocache_chan_write_count{0};
    std::atomic_uint64_t nocache_chan_write_failed{0};
    std::atomic_uint64_t read_count{0};
    std::atomic_uint64_t write_count{0};
    std::atomic_uint64_t write_failed{0};
    g_scheduler->Start();

    // 无缓冲channel
    bbt::co::sync::Chan<uint64_t, 0> chan;

    bbtco_desc("写者") [&]()
    {
        for (uint64_t i = 0; i < UINT64_MAX; ++i)
        {
            if (chan << i)
            {
                nocache_chan_write_count++;
            }
            else
            {
                nocache_chan_write_failed++;
            }
        }
    };

    bbtco_desc("读者") [&]()
    {
        uint64_t item;
        while (true)
        {
            Assert(chan >> item);
            nocache_chan_read_count++;
        }
    };

    // 有缓冲 channel
    bbt::co::sync::Chan<uint64_t, 10000> chan2;
    auto cond = bbtco_make_cocond();

    bbtco_desc("写者2") [&]()
    {
        while (true)
        {
            for (int i = 0; i < 10000; ++i)
            {
                if (chan2 << i)
                {
                    write_count++;
                }
                else
                {
                    write_failed++;
                }
            }
            cond->Wait();
        }
    };

    bbtco_desc("读者2") [&]()
    {
        uint64_t item;
        while (true)
        {
            for (int i = 0; i < 10000; ++i)
            {
                Assert(chan2 >> item);
                read_count++;
            }

            while (cond->NotifyOne() != -1)
                bbtco_sleep(5);
        }
    };


    bbtco_desc("监视者") [&]()
    {
        while (true) {
            printf("[nocache chan] read=%d write=%d write_failed=%d\n", nocache_chan_read_count.load(), nocache_chan_write_count.load(), nocache_chan_write_failed.load());
            printf("[chan] read=%d write=%d write_failed=%d\n", read_count.load(), write_count.load(), write_failed.load());
            sleep(1);
        }
    };
    
    sleep(100000);

    g_scheduler->Stop();
}