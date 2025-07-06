#include <libgo/libgo.h>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/core/clock/Clock.hpp>

const int max_task_count = 10000000;

int main()
{
    int work_thread = std::thread::hardware_concurrency();
    // 启动4个线程执行新创建的调度器
    std::thread t2([work_thread]{ g_Scheduler.Start(work_thread); });
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