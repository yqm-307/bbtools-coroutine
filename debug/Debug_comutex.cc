#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>

using namespace bbt::coroutine::sync;

void RunOnce()
{
    auto mutex = bbtco_make_comutex();
    const int nco_num = 100;
    const int nsec = 2000;
    int a = 0;
    int b = 0;

    const uint64_t begin = bbt::core::clock::gettime_mono();
    bbt::core::thread::CountDownLatch l{nco_num};

    for (int i = 0; i < nco_num; ++i) {
        bbtco [&mutex, &a, &b, begin, &l]() {
            while ((bbt::core::clock::gettime_mono() - begin) < nsec) {
                if (mutex->TryLock(1) == 0) {
                    Assert(a == b);
                    a++;
                    b++;
                    mutex->UnLock();
                }
            }
            l.Down();
        };
    }

    l.Wait();
}

void DeadLockAssert()
{
    auto mutex = bbtco_make_comutex();
    bbtco [&](){
        mutex->Lock();
        // mutex->Lock(); // assert fail
        // mutex->TryLock(); // assert fail
        // mutex->TryLock(1); // assert fail

        mutex->UnLock();
        mutex->Lock();      // success

        mutex->UnLock();
        mutex->TryLock();   // success

        mutex->UnLock();
        mutex->TryLock(1);  // success
    };

    sleep(1);
}

int main()
{
    g_scheduler->Start();

    // for (int i = 0; i < 100; ++i)
    //     RunOnce();

    DeadLockAssert();

    g_scheduler->Stop();
}