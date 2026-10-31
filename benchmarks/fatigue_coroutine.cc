#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

int main()
{
    std::atomic_uint64_t ncount = 0;
    g_scheduler->Start();

    while (true) {
        for (int i = 0; i < 100000; ++i) {
            bbtco ([&](){
                ncount++;
            });
        }

        sleep(1);
    }

    /* process-lifetime：无业务停机入口；业务完成后返回 main 即进程退出 */
}