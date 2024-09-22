#include <bbt/coroutine/coroutine.hpp>

int main()
{
    const int nco = 1000;
    auto lock = bbt::co::sync::StdLockWapper(bbtco_make_comutex());
    int a=0, b=0;

    g_scheduler->Start();

    std::array<std::atomic_uint64_t, nco> alive_ts_arr{bbt::clock::gettime()};

    for (auto&& it : alive_ts_arr)
        it = bbt::clock::gettime();


    for (int i = 0; i < nco; ++i) {
        bbtco [&, i](){
            while (true) {
                {
                    std::unique_lock<bbt::co::sync::StdLockWapper> _{lock};
                    alive_ts_arr[i] = bbt::clock::gettime();
                    Assert(a == b);
                    a++;
                    b++;
                }
                bbtco_sleep(2);
            }
        };
    }

    bbtco_desc("监视") [&](){
        while (true) {
            printf("checked\n");
            for (int i = 0; i < nco; ++i) {
                if (bbt::clock::gettime() - alive_ts_arr[i] > 2000) {
                    printf("co=%d diff=%d\n", i, bbt::clock::gettime() - alive_ts_arr[i]);
                    continue;
                }
            }
            sleep(1);
        }
    };

    sleep(60 * 60 * 2);

    g_scheduler->Stop();
}