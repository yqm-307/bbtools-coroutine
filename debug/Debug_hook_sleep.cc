#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/core/clock/Clock.hpp>
using namespace bbt::coroutine;


int main()
{
    std::atomic_int exit_value = 0;
    g_scheduler->Start();
    const int ncount = 1000;

    /* 两个协程注册时间相同。都执行休眠操作，唤醒事件相同，证明函数在sleep处无阻塞 */
    printf("begin time: %ld\n", bbt::core::clock::now<>().time_since_epoch().count());

    for (int i = 0; i < ncount; ++i)
    {
        g_scheduler->RegistCoroutineTask([i, &exit_value](){
            ::sleep(1);
            printf("tid:%ld t%d: %ld\n", ::gettid(), i, bbt::core::clock::now<>().time_since_epoch().count());
            exit_value++;
        });
    }


    while (exit_value.load() != ncount)
    {
        std::this_thread::sleep_for(bbt::core::clock::milliseconds(100));
    }

    g_scheduler->Stop();
    return 0;
}
