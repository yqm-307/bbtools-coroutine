#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
using namespace bbt::coroutine;

std::atomic_int g_count = 0;

int main()
{
    g_scheduler->Start();


    for (int i = 0; i < 10; ++i)
    {
        for (int i = 0; i < 5000; ++i)
        {
            g_scheduler->RegistCoroutineTask([](){
                g_count++;
                // printf("cur thread %ld %d\n", gettid(), g_count.load());
            });
        }
        std::this_thread::sleep_for(bbt::clock::milliseconds(50));
    }


    std::this_thread::sleep_for(bbt::clock::milliseconds(1000));
    if (g_count != 50000) {
        printf("final count: %d\n", g_count.load());
        g_scheduler->Stop();
        return -1;
    }

    g_scheduler->Stop();
    return 0;
}
