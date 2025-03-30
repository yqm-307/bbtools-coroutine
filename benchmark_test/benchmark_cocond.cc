#include <bbt/coroutine/coroutine.hpp>

using CoId = bbt::coroutine::detail::CoroutineId;

int main()
{
    const int ncount = 10000;
    const int time_s = 3600;


    std::map<CoId, int> co_sleep_count_map;
    std::mutex co_sleep_count_map_mutex;
    std::atomic_int stop_count{0};
    volatile bool is_stop{false};
    g_scheduler->Start();

    
    auto cocond = bbtco_make_cocond();

    for (int i = 0; i < ncount; ++i)
    {
        bbtco_desc("这个协程不停的 wait, 在醒来后将自己wait次数加1") [&](){
            while (true)
            {
                cocond->Wait();
                if (is_stop)
                {
                    stop_count++;
                    break;
                }
                std::unique_lock<std::mutex> lock(co_sleep_count_map_mutex);
                auto it = co_sleep_count_map.find(g_bbt_tls_coroutine_co->GetId());
                if (it == co_sleep_count_map.end())
                {
                    co_sleep_count_map[g_bbt_tls_coroutine_co->GetId()] = 1;
                }
                else
                {
                    it->second++;
                }
            }
        };
    }

    bbtco_desc("定时唤醒所有coroutine") [&](){
        int notify_times = 0;
        auto now = bbt::core::clock::gettime();
        while (now + time_s * 1000 > bbt::core::clock::gettime())
        {
            bbtco_sleep(200);
            cocond->NotifyAll();
            notify_times++;
            bbtco_sleep(200);
            std::unique_lock<std::mutex> lock(co_sleep_count_map_mutex);

            for (const auto& [co_id, count] : co_sleep_count_map)
            {
                if (count < notify_times)
                    printf("a bad co! co_id: %d, count: %d\n", co_id, count);
            }

            printf("notify all once! times: %d\n", notify_times);
        }
    };

    bbtco_desc("结束后销毁所有协程") [&](){
        bbtco_sleep(time_s * 1000);
        is_stop = true;
        while (stop_count.load() < ncount)
        {
            bbtco_sleep(100);
            cocond->NotifyAll();
        }
    };


    // 60s 回收所有资源
    sleep(time_s + 60);

    g_scheduler->Stop();
}