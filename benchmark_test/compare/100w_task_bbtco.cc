#include <bbt/coroutine/coroutine.hpp>

const int max_task_count = 1000000;
const int producer_count = 2;

int main()
{
    g_scheduler->Start();
    auto start = bbt::core::clock::now();

    bbt::core::thread::CountDownLatch latch(max_task_count);

    for (int i = 0; i < producer_count; ++i)
    {
        bbtco_desc("producer") [&latch]{
            for (int i = 0; i < max_task_count / producer_count; ++i)
            {
                bbtco [i,&latch]{
                    int a = i + i;
                    latch.Down();
                };
            }
        };
    }


    latch.Wait();
    g_scheduler->Stop();

    printf("cost time: %ld\n", (bbt::core::clock::now() - start).count());
}