#include <math.h>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

int main()
{
    /* 执行1000w个协程耗时 */
    const int nsum_co = 10000000;
    bbt::core::thread::CountDownLatch l{nsum_co};
    auto begin = bbt::core::clock::gettime();

    g_scheduler->Start();

    bbtco [&](){
        for (int i = 0; i < nsum_co; ++i) {
            bbtco [&](){ l.Down(); };
        }
    };

    l.Wait();
    g_scheduler->Stop();
    printf("time cost: %ldms\n", bbt::core::clock::gettime() - begin);
}