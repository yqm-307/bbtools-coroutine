#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>

using namespace bbt::coroutine::sync;

const int nco_num = 100;
auto mutex = bbtco_make_comutex();
volatile int a = 0;
volatile int b = 0;

void fatigue_1()
{
    for (int i = 0; i < nco_num; ++i) {
        bbtco []() {
            while (true) {
                mutex->Lock();
                Assert(a == b);
                a++; b++;
                mutex->UnLock();
            }
        };
    }

    // for (int i = 0; i < 5; ++i) {
    //     bbtco []() {
    //         while (true) {
    //             if (mutex.TryLock() == 0) {
    //                 Assert(a == b);
    //                 a++; b++;
    //                 mutex.UnLock();
    //             }
    //         }            
    //     };
    // }

    for (int i = 0; i < 5; ++i) {
        bbtco []() {
            while (true) {
                if (mutex->TryLock(10) == 0) {
                    Assert(a == b);
                    a++; b++;
                    mutex->UnLock();
                }
            }
        };
    }
    while(1) {
        printf("%d %d\n", a, b);
        sleep(1);
    }
}

int main()
{
    g_scheduler->Start();

    fatigue_1();

    g_scheduler->Stop();
}