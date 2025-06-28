#include <iostream>
#include <bbt/coroutine/coroutine.hpp>


void LimitCoroutineCount()
{
    g_bbt_coroutine_config->m_cfg_max_coroutine = 2;

    g_scheduler->Start();

    bool succ = false;
    bbtco_noexcept(&succ) [=]() {
        std::cout << "Coroutine 1" << std::endl;
        sleep(1);
    };

    if (!succ) {
        std::cerr << "Failed to register Coroutine 1" << std::endl;
    }

    for (int i = 2; i < 4; ++i) {
        bbtco_noexcept(&succ) [=]() {
            std::cout << "Coroutine " << i << std::endl;
            sleep(1);
        };
    
        if (!succ) {
            std::cerr << "Failed to register Coroutine " << i << std::endl;
        }
    }

    g_scheduler->Stop();
}

int main()
{
    LimitCoroutineCount();
}