#include <libgo/libgo.h>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/core/clock/Clock.hpp>

const int max_task_count = 1000000;

int main()
{
    // 启动4个线程执行新创建的调度器
    std::thread t2([]{ g_Scheduler.Start(4); });
    auto start = bbt::core::clock::now();

    bbt::core::thread::CountDownLatch latch(max_task_count);

    go [&latch]{
        for (int i = 0; i < max_task_count; ++i)
        {
            go [i, &latch]{
                int a = i + i;
                latch.Down();
            };
        }
    };

    latch.Wait();

    g_Scheduler.Stop();
    printf("cost time: %ld\n", (bbt::core::clock::now() - start).count());
    t2.join();
}