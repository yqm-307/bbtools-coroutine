#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

int main()
{
    std::atomic_uint64_t read_count{0};
    std::atomic_uint64_t write_count{0};
    std::atomic_uint64_t write_failed{0};
    g_scheduler->Start();

    /* 开启100个单读多写协程 */

    for (int i = 0; i < 100; ++i) {
        // chan读写协程
        bbtco [&read_count, &write_count, &write_failed](){
            sync::Chan<int, 65535> chan;
            for (int i = 0; i < 100; ++i) {
                bbtco [&chan, &write_count, &write_failed](){
                    while (true) {
                        for (int i = 0; i < 100; ++i) {
                            if (chan.Write(1) == 0)
                                write_count++;
                            else
                                write_failed++;
                        }
                        ::sleep(1);
                    }
                };
            }
            int val;

            while (true) {
                Assert(chan.Read(val) == 0);
                read_count++;
            }
        };
    }

    // 打印协程
    bbtco [&read_count, &write_count, &write_failed](){
        while (true) {
            printf("read=%d write=%d write_failed=%d\n", read_count.load(), write_count.load(), write_failed.load());
            sleep(1);
        }
    };

    while (1) sleep(1);

    g_scheduler->Stop();
}