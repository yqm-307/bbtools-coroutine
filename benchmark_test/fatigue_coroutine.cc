#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

int main()
{
    std::atomic_uint64_t ncount = 0;
    g_scheduler->Start(true);

    while (true) {
        for (int i = 0; i < 100000; ++i) {
            bbtco ([&](){
                ncount++;
            });
        }

        sleep(1);
    }

    g_scheduler->Stop();
}