#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/base/clock/Clock.hpp>
using namespace bbt::coroutine;


int main()
{
    std::atomic_int exit_value = 0;
    g_scheduler->Start(true);

    g_scheduler->RegistCoroutineTask([&](){
        ::sleep(1);
        printf("t1: %ld\n", bbt::clock::now<>().time_since_epoch().count());
        exit_value++;
    });

    g_scheduler->RegistCoroutineTask([&](){
        ::sleep(1);
        printf("t2: %ld\n", bbt::clock::now<>().time_since_epoch().count());
        exit_value++;
    });


    while (exit_value.load() != 2)
    {
        std::this_thread::sleep_for(bbt::clock::milliseconds(100));
    }    

    g_scheduler->Stop();
    return 0;
}
