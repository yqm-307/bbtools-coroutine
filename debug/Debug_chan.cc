#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
// #include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

void ReadOnce()
{

    bbtco [](){
        sync::Chan<int, 65535> a;
        bbtco [&a](){
            a.Write(1);
        };

        int val = 0;
        a.Read(val);
        printf("read value = %d\n", val);
    };

    sleep(1);

}

void ReadMulti()
{
    std::condition_variable cond;
    std::mutex              mutex;
    std::atomic_int         count = 0;

    // 注册一个协程
    bbtco [&count, &cond, &mutex](){
        sync::Chan<int, 65535> c;

        // 注册1000个写chan写成
        for (int i = 0; i < 1000; ++i) {
            bbtco [&c, i](){
                for (int i = 0; i < 1000; ++i) {
                    while(c.Write(i));
                    // int ret = ;
                    // Assert(ret == 0);
                }
            };
        }


        // 等待
        for (;count.load() < 1000*1000;) {
            std::vector<int> val;
            Assert(c.ReadAll(val) == 0);
            count += val.size();
        }
        
        cond.notify_one();
    };

    std::unique_lock<std::mutex> lock(mutex);
    auto begin = bbt::clock::now<>();
    cond.wait(lock);
    printf("chan写n次耗时：%ldms  count: %d\n", (bbt::clock::now<>() - begin).count(), count.load());
    count.exchange(0);

}

void DebugWriteBlock()
{
    bbt::thread::CountDownLatch l{1};
    const int nwrite = 100;
    std::atomic_int n_read_succ_num{0};
    std::set<int> results;

    bbtco [&](){
        auto chan = Chan<int, 1>();

        for (int i = 0; i < nwrite; ++i) {
            bbtco [&chan, &n_read_succ_num, i](){
                chan << i;
                n_read_succ_num++;
            };
        }

        bbt::coroutine::detail::Hook_Sleep(100);

        int val;
        for (int i = 0; i < nwrite; ++i) {
            while (!(chan >> val))
                detail::Hook_Sleep(1);
            results.insert(val);
        }

        l.Down();
    };

    l.Wait();

    Assert(nwrite == n_read_succ_num.load());

}

int main()
{
    g_scheduler->Start(true);
    ReadOnce();
    ReadMulti();
    DebugWriteBlock();
    g_scheduler->Stop();
}