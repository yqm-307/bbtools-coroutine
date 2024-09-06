#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
using namespace bbt::coroutine;

void dbg_regist_co()
{    
    for (int i = 0; i < 10000; ++i)
    {
        bbtco [=](){
            printf("当前协程id %lu 当前线程id %ld\n", GetLocalCoroutineId(), gettid());
        };
    }

    std::this_thread::sleep_for(bbt::clock::milliseconds(100));

}

void dbg_bbtco_yield()
{
    bbt::thread::CountDownLatch l{1};

    bbtco_yield;

    bbtco [&](){
        bbtco_yield;
        printf("1\n");
        bbtco_yield;
        printf("2\n");
        bbtco_yield;
        printf("3\n");
        bbtco_yield;
        printf("4\n");
        bbtco_yield;
        l.Down();
    };

    l.Wait();
}

int main()
{
    g_scheduler->Start();

    dbg_bbtco_yield();

    g_scheduler->Stop();
    return 0;
}
