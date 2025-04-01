#include <bbt/coroutine/coroutine.hpp>

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
    volatile bool writer_stop = false;
    volatile bool reader_stop = false;
    volatile bool monitor_stop = false;
    std::atomic_uint64_t read{0};
    std::atomic_uint64_t write{0};
    std::vector<WatchDog> dogs(10);
    g_scheduler->Start();

    auto chan = bbt::co::sync::Chan<uint64_t, 10000>();

    bbtco_desc("reader") [&](){
        uint64_t item;
        while (!reader_stop)
        {
            Assert(chan >> item);
            read++;
        }
    };

    for (int i = 0; i < 10; ++i)
    {
        bbtco_desc("writer") [&, i](){
            while (!writer_stop)
            {
                Assert(chan << 1);
                dogs[i].Feed();
                write++;
            }
        };
    }

    bbtco_desc("monitor") [&]()
    {
        while (!monitor_stop)
        {
            std::cout << "monitor read=" << read.load() << "\t write=" << write.load() << std::endl;
            for (int i = 0; i < 10; ++i)
            {
                if (dogs[i].IsDead())
                {
                    std::cout << "dog " << i << " is dead" << std::endl;
                }
            }
            bbtco_sleep(1000);
        }
    };

    sleep(10);
    writer_stop = true;
    sleep(2);
    reader_stop = true;
    sleep(2);
    monitor_stop = true;

    g_scheduler->Stop();
}