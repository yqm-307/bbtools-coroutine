#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/base/clock/Clock.hpp>
using namespace bbt::coroutine;


int main()
{
    std::atomic_int exit_value = 0;
    g_scheduler->Start(true);

    /* 两个协程注册时间相同。都执行休眠操作，唤醒事件相同，证明函数在sleep处无阻塞 */

    /* 注册一个协程 */
    g_scheduler->RegistCoroutineTask([&](){
        ::sleep(1);
        printf("t1: %ld\n", bbt::clock::now<>().time_since_epoch().count());
        exit_value++;
    });

    /* 注册一个协程 */
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
