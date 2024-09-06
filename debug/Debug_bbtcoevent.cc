#include <bbt/coroutine/coroutine.hpp>

int main()
{
    g_scheduler->Start();
    std::atomic_int val = 0;

    while (true) {
        
        for (int i = 0; i < 10000; ++i) {
            auto a = bbtco_event_regist(-1, bbt::pollevent::EventOpt::TIMEOUT, 100) [&](){
                val++;
            };

            Assert(a == 0);
        }

        sleep(1);
        printf("timenow:%ld ncount:%d\n", time(NULL), val.load());
    }

    g_scheduler->Stop();
}