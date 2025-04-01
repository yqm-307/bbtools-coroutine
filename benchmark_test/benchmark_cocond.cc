#include <bbt/coroutine/coroutine.hpp>

using CoId = bbt::coroutine::detail::CoroutineId;

class WatchDog
{
public:
    void Feed()
    {
        m_last_feed_time = bbt::core::clock::gettime();
    }

    bool IsDead()
    {
        auto now = bbt::core::clock::gettime();
        if (now - m_last_feed_time > 3 * 1000)
        {
            return true;
        }
        return false;
    }

private:
    std::atomic_uint64_t m_last_feed_time{bbt::core::clock::gettime()};
};

int main()
{
    const int ncount = 10000;
    const int time_s = 3600;

    std::vector<WatchDog> dogs(ncount);

    std::map<CoId, int> co_sleep_count_map;
    std::mutex co_sleep_count_map_mutex;
    std::atomic_int stop_count{0};
    volatile bool is_stop{false};
    g_scheduler->Start();

    
    auto cocond = bbtco_make_cocond();

    for (int i = 0; i < ncount; ++i)
    {
        bbtco_desc("这个协程不停的 wait, 在醒来后将自己wait次数加1") [&, i](){
            while (!is_stop)
            {
                cocond->Wait();
                dogs[i].Feed();
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
            stop_count++;
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

    bbtco_desc("monitor") [&]()
    {
        while (!is_stop)
        {
            bbtco_sleep(1000);
            std::cout << "monitor" << std::endl;
            for (int i = 0; i < ncount; ++i)
            {
                if (dogs[i].IsDead())
                {
                    std::cout << "dog " << i << " is dead" << std::endl;
                }
            }
        }
    };

    bbtco_desc("monitor") [&](){
        bbtco_sleep(time_s * 1000);
        is_stop = true;
        while (stop_count.load() < ncount)
        {
            bbtco_sleep(100);
            cocond->NotifyAll();
        }
    };


    // 5s 回收所有资源
    sleep(time_s + 5);

    g_scheduler->Stop();
}