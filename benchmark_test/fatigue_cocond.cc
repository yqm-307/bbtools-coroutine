#include <bbt/coroutine/coroutine.hpp>

int main()
{
    const int nco = 2000;
    g_scheduler->Start();

    /**
     * 开启n个协程不停挂起，在唤醒后标记自己的计数
     * 
     * 开启一个协程定时唤醒，并检查计数
     */

    std::array<std::atomic_int, nco> alive_arr{0};
    std::atomic_int current_frame{0};


    auto cond = bbtco_make_cocond();

    for (int i = 0; i < nco; ++i) {
        bbtco [&, i](){
            while (true) {
                alive_arr[i] = current_frame.load();
                cond->Wait();
            }
        };

    }

    bbtco_desc("定时检查") [&](){
        while (true) {
            printf("check: current=%d\n", current_frame.load());
            int total_count = 0;
            for (auto&& it : alive_arr) {
                // 10次未响应说明绝对被丢失了
                if (current_frame - it > 10) {
                    printf("co lose: %d\n", it.load());
                    continue;
                }
                total_count += it;
            }
            printf("total check count: %d\n", total_count);

            current_frame++;
            cond->NotifyAll();
            bbtco_sleep(1000);
        }
    };

    sleep(60 * 60 * 12); // 2小时检测

    g_scheduler->Stop();
}