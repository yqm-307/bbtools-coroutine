#include <math.h>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

void InitConfig()
{
    g_bbt_coroutine_config->m_cfg_stack_size = 1024 * 8; // 设置栈大小为8KB
    g_bbt_coroutine_config->m_cfg_stack_protect = true; // 启用栈保护
}

int main()
{
    InitConfig();
    /* 执行1000w个协程耗时 */
    const int nsum_co = 100000;
    std::atomic_int done_count{0};
    auto begin = bbt::core::clock::gettime();

    g_scheduler->Start();

    bbtco [&](){
        for (int i = 0; i < nsum_co; ++i) {
            bbtco [&](){ done_count++; };
        }
    };

    // l.Wait();

    while (done_count < nsum_co) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    g_scheduler->Stop();
    printf("time cost: %ldms\n", bbt::core::clock::gettime() - begin);
}