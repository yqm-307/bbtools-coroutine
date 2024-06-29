#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
// #include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

void ReadOnce()
{
    g_scheduler->Start(true);

    bbtco [](){
        sync::Chan<int> a;
        bbtco [&a](){
            a.Write(1);
        };

        int val = 0;
        a.Read(val);
        printf("read value = %d\n", val);
    };

    sleep(1);

    g_scheduler->Stop();
}

void ReadMulti()
{
    std::condition_variable cond;
    std::mutex mutex;

    g_scheduler->Start(true);
    std::atomic_int count = 0;
    bbtco [&count, &cond, &mutex](){
        sync::Chan<int> c{1000*1000};
        
        for (int i = 0; i < 1000; ++i)
        {
            bbtco [&c, i](){
                for (int i = 0; i < 1000; ++i) {
                    int ret = c.Write(i);
                    Assert(ret == 0);
                }
            };
        }

        for (int i = 0; i < 1000 * 1000; ++i) {
            int val;
            Assert(c.Read(val) == 0);
            count++;                                                                                                                                                                                    
        }
        
        cond.notify_one();
    };

    std::unique_lock<std::mutex> lock(mutex);
    auto begin = bbt::clock::now<>();
    cond.wait(lock);
    printf("chan写1000000次耗时：%ldms  count: %d\n", (bbt::clock::now<>() - begin).count(), count.load());
    g_scheduler->Stop();
}

int main()
{
    ReadOnce();
    ReadMulti();
}