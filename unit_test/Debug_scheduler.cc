#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
using namespace bbt::coroutine;

std::atomic_int g_count = 0;

int main()
{
    g_scheduler->Start(true);

    for (int i = 0; i < 1000; ++i)
    {
        g_scheduler->RegistCoroutineTask([](){
            g_count++;
            // printf("cur thread %ld %d\n", gettid(), g_count.load());
        });
    }


    sleep(2);
    if (g_count != 1000)
        return -1;

    g_scheduler->Stop();
    return 0;
}
