#include <bbt/coroutine/coroutine.hpp>

std::mutex mutex;

int main()
{
    /* 线程级别的waitgroup */
    bbt::thread::CountDownLatch l{1};

    g_scheduler->Start();

    /* 启动main协程 */
    bbtco_desc("main") [&](){

        /* 启动cond wait协程 */
        bbtco_desc("wait co") [](){

            /* 创建cond */
            auto cond = bbtco_make_cocond(mutex);

            /* 启动cond notify 协程 */
            bbtco_desc("notify co") [&](){
                /* 休眠尽量保证挂起了 */
                bbtco_sleep(10);
                printf("notify one! co=%ld\n", bbt::coroutine::GetLocalCoroutineId());
                cond->NotifyOne();
            };

            /* 创建后挂起 */
            printf("cond wait! co=%ld\n", bbt::coroutine::GetLocalCoroutineId());
            cond->Wait();
            printf("cond notified! co=%ld\n", bbt::coroutine::GetLocalCoroutineId());
        };

        bbtco_sleep(100);
        l.Down();
    };

    l.Wait();
    g_scheduler->Stop();
}