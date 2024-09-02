#include <math.h>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>

int arithmetic()
{
    int a = 0;
    // for (int i = 0; i < 1000; ++i) {
    //     a = rand() % 1000;
    //     a = sqrt(a);
    // }

    return a;
}

int main()
{
    int val = 0;
    const int nsum_co = 10000000;
    bbt::thread::CountDownLatch l{nsum_co};

    bbtco [&](){

        for (int i = 0; i < nsum_co; ++i)
            bbtco [&](){ val = arithmetic(); l.Down(); };
        
        l.Wait();
        g_scheduler->Stop();
    };

    g_scheduler->Start();
}