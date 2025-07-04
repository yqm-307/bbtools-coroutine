#include <math.h>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

void InitConfig()
{
    g_bbt_coroutine_config->m_cfg_stack_size = 1024 * 8; // 设置栈大小为8KB
    g_bbt_coroutine_config->m_cfg_stack_protect = true; // 启用栈保护
    g_bbt_coroutine_config->m_cfg_max_coroutine = 10000;
}

int main()
{
    InitConfig();
    /* 执行1000w个协程耗时 */
    const int nsum_co = 1000000;
    std::atomic_int done_count{0};
    auto begin = bbt::core::clock::gettime();
    int productor_num = std::thread::hardware_concurrency() / 2 > 1 ? std::thread::hardware_concurrency() / 2 : 1; 

    g_scheduler->Start();

    for (int i = 0; i < productor_num; ++i)
    {
        bool succ = false;
        while (!succ)
        {
            // productor
            bbtco_noexcept(&succ) [&done_count, nsum_co, i, productor_num](){
                
                for (int j = 0; j < (nsum_co / productor_num) + 1; ++j) {
                    bool succ = false;
                    
                    while (!succ)
                    {
                        bbtco_noexcept(&succ) [&done_count, j](){
                            done_count++;
                        };
                        
                        if (!succ)
                            bbtco_sleep(1);
                    }
                    
                }
            };
        }
    }


    // for (int i = 0; i < producer; ++i) {
    //     bool succ = false;
    //     while (!succ)
    //     {
    //         bbtco_noexcept(&succ) [&done_count, nsum_co, i, producer](){
    //             for (int j = 0; j < (nsum_co / producer) + 1; ++j) {
    //                 bbtco [&done_count, j](){
    //                     done_count++;
    //                 };
    //             }
    //         };
    //     }
    // }

    // l.Wait();

    while (done_count < nsum_co) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    g_scheduler->Stop();
    printf("time cost: %ldms\n", bbt::core::clock::gettime() - begin);
}