#include <cstdio>
#include <memory>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoLockGuard.hpp>

using namespace bbt::coroutine;

int bbtco_main()
{
    auto mutex = bbtco_make_comutex();
    auto count = std::make_shared<int>(0);
    auto done = Chan<int, 1>();
    auto increment = [mutex, count]() {
        sync::CoLockGuard<sync::CoMutex> lock(mutex);
        return ++*count;
    };
    bbtco [increment, done]() {
        (void)done->TryWrite(increment());
    };
    const int local = increment();
    int remote = 0;
    if (done->Read(remote) != 0)
        return 1;

    auto rwmutex = bbtco_make_corwmutex();
    int snapshot = 0;
    {
        sync::CoWriteLock lock(rwmutex);
        snapshot = local + remote;
    }
    {
        sync::CoReadLock lock(rwmutex);
        std::printf("snapshot: %d\n", snapshot);
    }
    return 0;
}

BBTCO_MAIN()
