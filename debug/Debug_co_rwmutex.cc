#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;



void rwlock_test()
{
    const int nread_co = 200;
    const int nwrite_co = 2;
    std::array<int, nread_co> vec;
    int n = 0;
    int m = 0;

    bbtco [&](){
        auto rwlock = sync::CoRWMutex::Create();

        for (int i = 0; i < nread_co; ++i) {
            bbtco [&, rwlock, i](){            
                while (true) {
                    rwlock->RLock();
                    Assert(n == m);
                    vec[i]++;
                    rwlock->UnLock();
                    // bbtco_sleep(1);
                    bbtco_yield;
                }    
            };
        }

        for (int i = 0; i < nwrite_co; ++i) {
            bbtco [&, rwlock, i](){
                while (true) {
                    rwlock->WLock();
                    Assert(n == m);
                    n++;
                    m++;
                    rwlock->UnLock();
                    bbtco_yield;
                }
            };
        }

        bbtco [&](){
            while (true) {
                printf("\n=====================================\n");
                for (auto v : vec) {
                    printf("%d\n", v);
                }

                sleep(1);
            }
        };
    };

    while (true) sleep(1);
    
}

int main()
{
    g_scheduler->Start();

    rwlock_test();

    g_scheduler->Stop();
}