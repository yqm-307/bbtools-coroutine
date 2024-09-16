#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

int main()
{
    const int nsum_co = 1000000;

    g_scheduler->Start();

    auto pool = bbtco_make_copool(3000);

    while (true)
    {   
        for (int i = 0; i < nsum_co; ++i)
            pool->Submit([&](){});
        sleep(1);
    }


    pool->Release();
    g_scheduler->Stop();

}