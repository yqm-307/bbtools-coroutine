#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

void Normal()
{
    bbtco [](){
        printf("i am coroutine!\n");
    };

    sleep(1);
}

// sleep的行为在普通线程是系统函数行为，但是在Processer线程行为是挂起协程
void SleepHook()
{
    printf("SleepHook begin\n");

    bbtco [](){
        printf("[%d] sleep before!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
        sleep(1);
        printf("[%d] sleep end!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
    };

    bbtco [](){
        printf("[%d] sleep before!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
        sleep(1);
        printf("[%d] sleep end!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
    };

    sleep(2);

    printf("SleepHook end\n");
}

int main()
{
    g_scheduler->Start();

    Normal();

    SleepHook();

    g_scheduler->Stop();
}