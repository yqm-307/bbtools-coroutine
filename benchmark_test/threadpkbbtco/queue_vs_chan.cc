#include <queue>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>

const int queue_count = 1000 * 10000;

// 多线程队列加锁
void Case_MultiThreadQueue()
{
    bbt::core::thread::CountDownLatch l{1};

    std::thread *consumer, *producter = nullptr;
    std::mutex lock;
    int cost = 0;
    std::queue<int> queue;

    auto start = std::chrono::steady_clock::now();

    producter = new std::thread([&](){
        for (int i = 0; i < queue_count; ++i)
        {
            std::lock_guard<std::mutex> _(lock);
            queue.push(1);
        }
    });

    consumer = new std::thread([&](){
        cost = 0;
        while (cost < queue_count)
        {
            std::lock_guard<std::mutex> _(lock);
            if (!queue.empty())
            {
                queue.pop();
                cost++;
            }
        }
        l.Down();
    });

    l.Wait();

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "MultiThreadQueue cost: " << duration << " ms" << "\tcost: " << cost << std::endl;
    if (producter->joinable())
        producter->join();
    if (consumer->joinable())
        consumer->join();
    delete producter;
    delete consumer;
}

// 多协程chan
void Case_MultiCoChan()
{
    bbt::core::thread::CountDownLatch l{1};

    auto chan = bbt::co::Chan<int, 100000>();
    int cost = 0;
    int product_count = 0;
    auto start = std::chrono::steady_clock::now();

    // bbtco_desc("monitor") [&](){
    //     while (true)
    //     {
    //         printf("MultiCoChan start, cost: %d product_count: %d\n", cost, product_count);
    //         bbtco_sleep(1000);
    //     }
    // };

    bbtco_desc("producter") [&](){
        for (int i = 0; i < queue_count;)
        {
            if (chan->Write(1) == 0)
            {
                i++;
                product_count++;
            }
        }
    };

    bbtco_desc("consumer") [&](){
        cost = 0;
        while (cost < queue_count)
        {
            int item = 0;
            if (chan->IsClosed())
                break;

            if (chan->Read(item) == 0)
            {
                cost++;
            }
        }
        l.Down();
    };

    l.Wait();
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "MultiCoChan cost: " << duration << " ms" << "\tcost " << cost << std::endl;

}

void Case_MultiCoMutex()
{
   bbt::core::thread::CountDownLatch l{1};

    // auto chan = bbt::co::Chan<int, 100000>();
    auto comutex = bbtco_make_comutex();
    std::queue<int> queue;
    auto start = std::chrono::steady_clock::now();

    bbtco_desc("producter") [&](){
        for (int i = 0; i < queue_count; ++i)
        {
            comutex->Lock();
            queue.push(1);
            comutex->UnLock();
        }
    };

    bbtco_desc("consumer") [&](){
        int cost = 0;
        while (cost < queue_count)
        {
            comutex->Lock();
            if (!queue.empty())
            {
                queue.pop();
                cost++;
            }
            comutex->UnLock();
        }
        l.Down();
    };

    l.Wait();
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "MultiCoMutex cost: " << duration << " ms"<< std::endl;
}

int main()
{
    Case_MultiThreadQueue();
    
    g_scheduler->Start();
    Case_MultiCoChan();
    Case_MultiCoMutex();
    g_scheduler->Stop();

}