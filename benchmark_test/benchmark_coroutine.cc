#include <math.h>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

int main()
{
    /* 执行1000w个协程耗时 */
    const int nsum_co = 10000000;
    bbt::thread::CountDownLatch l{nsum_co};
    auto begin = bbt::clock::gettime();

    g_scheduler->Start();

    bbtco [&](){
        for (int i = 0; i < nsum_co / 10000; ++i) {
            for (int j = 0 ; j < 10000; ++j)
                bbtco [&](){ l.Down(); };
            bbtco_sleep(1000);
        }
    };

    l.Wait();
    g_scheduler->Stop();
    printf("time cost: %ldms\n", bbt::clock::gettime() - begin);
}