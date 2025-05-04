#include <math.h>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

int main()
{
    /* 执行1000w个协程耗时 */
    const int nsum_co = 10000000;
    std::atomic_int done_count{0};
    auto begin = bbt::core::clock::gettime();

    g_scheduler->Start();

    bbtco [&](){
        for (int i = 0; i < nsum_co; ++i) {
            bbtco [&](){ done_count++; };
        }
    };

    // l.Wait();

    while (done_count < nsum_co) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    g_scheduler->Stop();
    printf("time cost: %ldms\n", bbt::core::clock::gettime() - begin);
}