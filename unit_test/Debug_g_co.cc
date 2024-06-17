#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
using namespace bbt::coroutine;


int main()
{
    g_scheduler->Start(true);

    for (int i = 0; i < 1000; ++i)
    {
        g_scheduler->RegistCoroutineTask([=](){
            printf("当前协程id %lu 当前线程id %ld\n", GetLocalCoroutineId(), gettid());
        });
    }

    std::this_thread::sleep_for(bbt::clock::milliseconds(100));

    g_scheduler->Stop();
    return 0;
}
