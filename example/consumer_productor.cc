#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <atomic>
#include <memory>

using namespace bbt::coroutine;

// 任务结构
struct Task {
    int id;
    std::string data;
    
    Task(int i, const std::string& d) : id(i), data(d) {}
};

// 生产者-消费者示例
void ProducerConsumerExample()
{
    // 创建任务通道
    auto task_chan = sync::Chan<Task, 10>();
    
    // 创建协程池处理任务
    auto worker_pool = bbtco_make_copool(3);
    
    // 统计变量
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    
    // 创建生产者协程
    for (int i = 0; i < 2; ++i) {
        bbtco_desc("producer") [&, i](){
            for (int j = 0; j < 5; ++j) {
                Task task(i * 100 + j, "data_" + std::to_string(i) + "_" + std::to_string(j));
                
                printf("Producer %d: creating task %d\n", i, task.id);
                task_chan << task;
                
                produced++;
                printf("Producer %d: task %d sent, total produced: %d\n", 
                       i, task.id, produced.load());
                
                // 模拟生产间隔
                bbtco_sleep(10);
            }
            printf("Producer %d: finished producing tasks\n", i);
        };
    }
    
    // 创建消费者协程
    for (int i = 0; i < 3; ++i) {
        bbtco_desc("consumer") [&, i](){
            while (true) {
                Task task(0, "");
                
                // 尝试从通道读取任务
                if (!(task_chan >> task)) {
                    printf("Consumer %d: channel closed, exiting\n", i);
                    break;
                }
                
                printf("Consumer %d: processing task %d (%s)\n", 
                       i, task.id, task.data.c_str());
                
                // 提交任务到协程池处理
                worker_pool->Submit([task, i, &consumed]() {
                    // 使用defer确保计数器更新
                    bbtco_defer {
                        consumed++;
                        printf("Task %d processed by consumer %d, total consumed: %d\n", 
                               task.id, i, consumed.load());
                    };
                    
                    // 模拟处理时间
                    bbtco_sleep(10);
                    
                    printf("Task %d completed successfully\n", task.id);
                });
            }
        };
    }
    
    // 监控协程
    bbtco_desc("monitor") [&](){
        while (produced < 10 || consumed < produced) {
            printf("=== Status: Produced=%d, Consumed=%d ===\n", 
                   produced.load(), consumed.load());
            bbtco_sleep(500);
        }
    };
    
    // 等待生产完成
    bbtco_desc("closer") [&](){
        // 等待所有生产者完成
        while (produced < 10) {
            bbtco_sleep(100);
        }
        
        printf("All production completed, closing channel\n");
        task_chan.Close();
    };
    
    // 等待所有任务完成
    sleep(3);
    
    // 等待协程池完成所有任务
    worker_pool->Release();
    
    printf("Final status: Produced=%d, Consumed=%d\n", 
           produced.load(), consumed.load());
}

int main()
{
    printf("=== bbtools-coroutine Comprehensive Example ===\n");
    
    // 启动调度器
    g_scheduler->Start();
    
    // 运行示例
    ProducerConsumerExample();
    
    // 停止调度器
    g_scheduler->Stop();
    
    printf("Example completed successfully!\n");
    return 0;
}