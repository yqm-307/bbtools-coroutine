#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

int main()
{
    const int nsum_co = 10000000;
    bbt::thread::CountDownLatch l{nsum_co};
    auto begin = bbt::clock::gettime();

    g_scheduler->Start();

    pool::CoPool pool{100};

    bbtco [&](){
        for (int i = 0; i < nsum_co; ++i) {
            pool.Submit([&](){ l.Down(); });
        }
    };

    l.Wait();

    pool.Release();

    g_scheduler->Stop();
    printf("time cost: %ldms\n", bbt::clock::gettime() - begin);

}