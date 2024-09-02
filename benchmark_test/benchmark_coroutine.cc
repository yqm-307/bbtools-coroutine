#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>

int main()
{
    int ncount = 0;
    g_scheduler->Start(true);


    for (int i = 0; i<2; ++i)
    {
        new std::thread([&ncount](){
            while (true)
            {
                bbtco ([&](){
                    ncount++;
                });
            }
        });
    }
    
    sleep(120);
    g_scheduler->Stop();
}