#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
using namespace bbt::coroutine;


#define PrintTime(flag) printf("标记点=[%s]   协程id=[%ld] 时间戳=[%ld]\n", flag, GetLocalCoroutineId(), bbt::clock::now<>().time_since_epoch().count());

void debug_notify()
{
    /* 启动调度线程、Processer线程 */
    g_scheduler->Start();

    printf("=============================================================\n");
    printf("======================    notify    ========================\n");
    printf("=============================================================\n");
    PrintTime("t1");

    /* 注册一个 coroutine */
    bbtco [](){
        PrintTime("t2");
        /* 创建条件变量 */
        auto cond = sync::CoWaiter::Create();
        Assert(cond != nullptr);

        /* 注册一个coroutine */
        bbtco [cond](){
            PrintTime("t4");
            /* 协程休眠1s后唤醒等待中的条件变量 */
            ::sleep(1);
            cond->Notify();
        };

        PrintTime("t3");
        /* 让当前协程等待条件变量变化 */
        Assert(cond->Wait() == 0);
        PrintTime("t5");

        /* 注册一个coroutine */
        bbtco [cond](){
            PrintTime("t6");
            /* 协程休眠1s后唤醒等待中的条件变量 */
            ::sleep(1);
            cond->Notify();
        };
        Assert(cond->Wait() == 0);
        PrintTime("t7");
    };

    std::this_thread::sleep_for(bbt::clock::milliseconds(4000));

    printf("=============================================================\n");
    printf("====================== wait timeout  ========================\n");
    printf("=============================================================\n");

    bbtco [](){
        PrintTime("p1");
        auto cond = sync::CoWaiter::Create();
        Assert(cond != nullptr);
        int ret = cond->WaitWithTimeout(1000);
        PrintTime("p2");
        printf("WaitRet=%d\n", ret);
    };

    std::this_thread::sleep_for(bbt::clock::milliseconds(2000));

    g_scheduler->Stop();
}

// 协程挂起功能是否有问题
void dbg_coroutine_wait()
{
    auto cond = sync::CoWaiter::Create();
    for (int i = 0; i < 10; ++i)
        bbtco [&](){
            while (true)
                cond->WaitWithTimeout(1);
        };
    ::sleep(1);
    
    bbtco [&](){
        while (1) {
            cond->Notify();
        }
    };

    while (true) sleep(1);
    

}

void cocond()
{
    std::mutex lock;
    bbt::thread::CountDownLatch l{1};
    bbt::thread::CountDownLatch l2{1};

    bbtco [&](){
        auto cond = sync::CoCond::Create(lock);

        bbtco [&](){
            cond->Wait();
            l.Down();
        };

        sleep(1);
        cond->NotifyOne();
        l.Wait();

        l2.Down();
    };

    l2.Wait();
}

void multi_cond_check()
{
    std::mutex lock;
    const int nmax_wait_co = 1000;
    bbt::thread::CountDownLatch l1{1};
    bbt::thread::CountDownLatch l2{1};

    for (int i = 0; i < 12000; ++i) {
        l1.Reset(nmax_wait_co);
        l2.Reset(1);
        std::atomic_int wait_count{0};
        bbtco [&, i](){
            auto cond = sync::CoCond::Create(lock);

            for (int i = 0; i < nmax_wait_co; ++i) {
                bbtco [&](){
                    wait_count++;
                    cond->Wait();
                    l1.Down();
                };
            }

            while (wait_count < nmax_wait_co)
                bbtco_sleep(10);
            
            cond->NotifyAll();
            while (0 != l1.WaitTimeout(10));
            l2.Down();
            printf("done %d turn!\n", i);
        };

        l2.Wait();
    }
}

void consumer_producer()
{
    std::mutex lock;
    bbtco [&](){
        auto cond = sync::CoCond::Create(lock);

        bbtco_desc("this is consumer co!") 
        [&](){
            while(true) {
                cond->NotifyAll();
                bbtco_sleep(1);
            }
        };

        bbtco_desc("this is producer co!")
        [&](){
            while (true) {
                for (int i = 0; i < 200; ++i)
                    bbtco_desc("products") [&](){
                        cond->Wait();
                    };
                
                bbtco_sleep(5);
            }
        };

        while(true) sleep(1);
    };

    while(true) sleep(1);
}

int main()
{
    g_scheduler->Start();

    // debug_notify();
    // dbg_coroutine_wait();
    // cocond();
    multi_cond_check();
    // consumer_producer();

    g_scheduler->Stop();
}
