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
    std::mutex              mutex;
    std::atomic_int         count = 0;

    // 启动调度器
    g_scheduler->Start(true);
    
    for (int i = 0; i < 1000; ++i) {
        // 注册一个协程
        bbtco [&count, &cond, &mutex](){
            sync::Chan<int> c{1000*1000};

            // 注册1000个写chan写成
            for (int i = 0; i < 10000; ++i) {
                bbtco [&c, i](){
                    for (int i = 0; i < 1000; ++i) {
                        while(c.Write(i));
                        // int ret = ;
                        // Assert(ret == 0);
                    }
                };
            }


            // 等待
            for (;count.load() < 1000*10000;) {
                std::vector<int> val;
                Assert(c.ReadAll(val) == 0);
                count += val.size();
            }
            
            cond.notify_one();
        };

        std::unique_lock<std::mutex> lock(mutex);
        auto begin = bbt::clock::now<>();
        cond.wait(lock);
        printf("chan写10000000次耗时：%ldms  count: %d\n", (bbt::clock::now<>() - begin).count(), count.load());
        count.exchange(0);
    }

    g_scheduler->Stop();
}

int main()
{
    // ReadOnce();
    ReadMulti();
}