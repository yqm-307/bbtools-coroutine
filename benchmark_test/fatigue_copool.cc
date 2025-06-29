#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

int main()
{
    const int nsum_co = 1000000;
    std::atomic_int count = 0;
    g_scheduler->Start();

    bbtco [&](){
        while (true) {
            printf("count= %d\n", count.load());
            sleep(1);
        }

    };

    while (true)
    {
        auto pool = bbtco_make_copool(100);

        for (int i = 0; i < nsum_co; ++i)
            pool->Submit([&](){
                count++;
            });

        count++;
    }

    g_scheduler->Stop();

}