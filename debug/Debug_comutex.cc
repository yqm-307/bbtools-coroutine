#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>

using namespace bbt::coroutine::sync;

void RunOnce()
{
    CoMutex mutex;
    const int nco_num = 100;
    const int nsec = 2000;
    int a = 0;
    int b = 0;

    const uint64_t begin = bbt::clock::gettime_mono();
    bbt::thread::CountDownLatch l{nco_num};

    for (int i = 0; i < nco_num; ++i) {
        bbtco [&mutex, &a, &b, begin, &l]() {
            while ((bbt::clock::gettime_mono() - begin) < nsec) {
                if (mutex.TryLock(1) == 0) {
                    Assert(a == b);
                    a++;
                    b++;
                    mutex.UnLock();
                }
            }
            l.Down();
        };
    }

    l.Wait();
}

int main()
{
    g_scheduler->Start();

    for (int i = 0; i < 100; ++i)
        RunOnce();

    g_scheduler->Stop();
}