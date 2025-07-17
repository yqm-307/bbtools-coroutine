#include <bbt/coroutine/coroutine.hpp>
#include <array>
#include <type_traits>

using namespace bbt::coroutine;

/**
 * 同时存在10个10个协程的池子，每秒执行10w次任务
 * 
 * 
 * 执行12个小时
 */

int main()
{
    const int m_once_co_num = 100000;
    g_scheduler->Start();

    const int time_s = 12 * 60 * 60; // 12小时
    bbt::core::clock::Timestamp<> end_time = bbt::core::clock::now<>() + bbt::core::clock::s(time_s);

    while (bbt::core::clock::now() < end_time)
    {
        bbt::core::thread::CountDownLatch latch{10 * m_once_co_num};
        std::array<decltype(bbtco_make_copool(10)), 10> m_pools;

        for (int i = 0; i < 10; ++i)
        {
            m_pools[i] = bbtco_make_copool(10);
            for (int j = 0; j < m_once_co_num; ++j) {
                m_pools[i]->Submit([&](){
                    latch.Down();
                });
            }
        }

        // 执行完毕后，清楚所有资源
        latch.Wait();

        for (int i = 0; i < 5; ++i) {
            m_pools[i]->Release();
        }
        for (int i = 5; i < 10; ++i) {
            m_pools[i] = nullptr; // 释放资源
        }

        sleep(1);
        std::cout << "All tasks completed in pool! current_time: " << bbt::core::clock::now<>().time_since_epoch().count() << std::endl;

    }

    g_scheduler->Stop();

}