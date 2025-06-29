#include <bbt/coroutine/coroutine.hpp>

const int max_task_count = 1000000;
const int producer_count = 2;

void Init()
{
    g_bbt_coroutine_config->m_cfg_stack_size = 1024 * 8; // 设置栈大小为8KB
    g_bbt_coroutine_config->m_cfg_stack_protect = false; // 启用栈保护
    g_bbt_coroutine_config->m_cfg_static_coroutine = 10000;
}

int main()
{
    Init();
    g_scheduler->Start();
    auto start = bbt::core::clock::now();

    bbt::core::thread::CountDownLatch latch(max_task_count);

    for (int i = 0; i < producer_count; ++i)
    {
        bbtco_desc("producer") [&latch]{
            for (int i = 0; i < max_task_count / producer_count; ++i)
            {
                bool succ = false;
                while (!succ)
                {
                    bbtco_noexcept(&succ) [i,&latch]{
                        int a = i + i;
                        latch.Down();
                    };

                    if (!succ)
                    {
                        // 如果注册失败，说明协程池满了，等待一段时间后重试
                        bbtco_sleep(1);
                    }
                }
            }
        };
    }


    latch.Wait();
    g_scheduler->Stop();

    printf("cost time: %ld\n", (bbt::core::clock::now() - start).count());
}