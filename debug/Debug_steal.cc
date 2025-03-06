#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
using namespace bbt::coroutine;

int main()
{
    int ncount;
    g_scheduler->Start();

    for (int i = 0; i<2; ++i)
    {
        bbtco [&ncount](){
            while (true)
            {
                bbtco ([&](){
                    ncount++;
                });
            }
        };
    }
    
    while(true) 
        sleep(1);

    g_scheduler->Stop();
}