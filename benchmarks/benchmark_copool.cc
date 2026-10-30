#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

int main()
{
    const int nsum_co = 10000000;
    bbt::core::thread::CountDownLatch l{nsum_co};
    auto begin = bbt::core::clock::gettime();

    g_scheduler->Start();

    auto pool = bbtco_make_copool(100);

    bbtco [&](){
        for (int i = 0; i < nsum_co; ++i) {
            pool->Submit([&](){ l.Down(); });
        }
    };

    l.Wait();


    /* process-lifetime：无业务停机入口；业务完成后返回 main 即进程退出 */
    printf("time cost: %ldms\n", bbt::core::clock::gettime() - begin);

}