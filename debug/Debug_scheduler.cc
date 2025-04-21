#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
using namespace bbt::coroutine;

std::atomic_int g_count = 0;

void SchedulerThread()
{
    g_scheduler->Start();


    for (int i = 0; i < 10; ++i)
    {
        for (int i = 0; i < 5000; ++i)
        {
            g_scheduler->RegistCoroutineTask([](){
                g_count++;
                // printf("cur thread %ld %d\n", gettid(), g_count.load());
            });
        }
        std::this_thread::sleep_for(bbt::core::clock::milliseconds(50));
    }


    std::this_thread::sleep_for(bbt::core::clock::milliseconds(1000));
    if (g_count != 50000) {
        printf("final count: %d\n", g_count.load());
        g_scheduler->Stop();
        return;
    }

    g_scheduler->Stop();
}

void LoopOnce()
{
    g_scheduler->Start(bbt::coroutine::SCHE_START_OPT_SCHE_NO_LOOP);

    bbtco [](){
        printf("i can print!\n");
    };

    bbtco [](){
        printf("i can print!\n");
        bbtco_sleep(10);
        printf("can`t go there!\n");
    };

    // LoopOnce驱动CoPoller处理事件
    // sleep(1);
    // g_scheduler->LoopOnce();
    sleep(1);

    g_scheduler->Stop();
}

void SetThreadCount()
{
    g_bbt_coroutine_config->m_cfg_static_thread_num = 1;
    g_scheduler->Start();


    sleep(10);
    g_scheduler->Stop();
}

int main()
{
    // LoopOnce();
    SetThreadCount();
    return 0;
}
