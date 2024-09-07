#include <bbt/coroutine/coroutine.hpp>

int main()
{
    g_scheduler->Start();
    std::atomic_int val = 0;

    while (true) {
        
        for (int i = 0; i < 10000; ++i) {
            auto a = bbtco_ev_t(100) [&](int fd, short event){
                val++;
            };

            Assert(a == 0);
        }

        std::this_thread::sleep_for(bbt::clock::ms(100));
        printf("timenow:%ld ncount:%d\n", time(NULL), val.load());
    }

    g_scheduler->Stop();
}