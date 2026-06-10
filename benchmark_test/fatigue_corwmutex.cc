#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoRWMutex.hpp>

using namespace bbt::coroutine::sync;

const int nread_co = 100;
const int nwrite_co = 5;
auto rwlock = bbtco_make_corwmutex();
volatile int nread = 0;
volatile int nwrite = 0;
volatile int in_writing = 0;

void fatigue_rwmutex()
{
    for (int i = 0; i < nread_co; ++i) {
        bbtco []() {
            while (true) {
                rwlock->RLock();
                Assert(in_writing == 0);
                nread++;
                rwlock->RUnLock();
                bbtco_yield;
            }
        };
    }

    for (int i = 0; i < nwrite_co; ++i) {
        bbtco []() {
            while (true) {
                rwlock->WLock();
                in_writing = 1;
                Assert(in_writing == 1);
                nwrite++;
                in_writing = 0;
                rwlock->WUnLock();
                bbtco_yield;
            }
        };
    }

    for (int i = 0; i < 5; ++i) {
        bbtco []() {
            while (true) {
                if (rwlock->TryRLock() == 0) {
                    Assert(in_writing == 0);
                    rwlock->RUnLock();
                }
                bbtco_yield;
            }
        };
    }

    bbtco []() {
        while (true) {
            printf("[fatigue_corwmutex] rlock=%d wlock=%d\n", nread, nwrite);
            sleep(1);
        }
    };
}

int main()
{
    g_scheduler->Start();

    fatigue_rwmutex();

    g_scheduler->Stop();
}
