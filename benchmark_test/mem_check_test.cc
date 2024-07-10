#include <cmath>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/base/random/Random.hpp>
#include <bbt/coroutine/coroutine.hpp>

int a = 0;
bbt::random::mt_random rd;

void _CoroutineTask()
{
    a = (int)sqrt(rd() % 1000) * 100 - 42;
}

void CoTask()
{
    const int max = 10000;
    std::atomic_int count{0};
    bbtco [&count](){
        for (int i = 0; i < max; ++i) {
            bbtco [&count](){
                _CoroutineTask();
                count++;
            };
        }
    };

    while (count < max) {
        std::this_thread::sleep_for(bbt::clock::milliseconds(10));
    }

}

void ChanRW()
{
    const int write_times = 10000;
    const int writer_num = 5;
    const int reader_num = 10;
    bbt::thread::CountDownLatch l{reader_num};
    std::atomic_int count{0};

    for (int i = 0; i < reader_num; ++i) {
        bbtco [&l]() {
            auto chan = std::make_shared<bbt::coroutine::sync::Chan<int>>();
            for (int j = 0; j < writer_num; ++j) {
                bbtco [=](){
                    for (int k = 0; k < write_times; ++k) {
                        while (chan->Write(1) != 0);
                    }
                };
            }

            int read_num = 0;
            while (true) {
                if (read_num >= (write_times * writer_num))
                    break;
                
                int val;
                Assert(chan->Read(val) == 0);
                read_num++;
            }
            l.Down();
        };
    }

    l.Wait();

}

int main()
{
    g_scheduler->Start(true);
    // 协程执行测试
    CoTask();
    // Chan执行测试
    ChanRW();
    g_scheduler->Stop();
}