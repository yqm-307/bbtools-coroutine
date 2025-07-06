#include <bbt/coroutine/coroutine.hpp>
#include <atomic>
#include <memory>

using namespace bbt::coroutine;

void ConsumerProblems()
{
    auto comutex = bbtco_make_comutex();
    
    // 共享资源
    std::shared_ptr<int> shared_data = std::make_shared<int>(0);
    std::atomic<int> reader_count{0};
    std::atomic<int> writer_count{0};
    
    // 创建多个读者协程
    for (int i = 0; i < 5; ++i) {
        bbtco_desc("reader") [&, i](){
            for (int j = 0; j < 10; ++j) {
                // 读者获取锁
                comutex->Lock();
                
                reader_count++;
                printf("Reader %d: 开始读取，当前读者数量: %d\n", i, reader_count.load());
                
                // 模拟读取操作
                int value = *shared_data;
                printf("Reader %d: 读取到值 %d\n", i, value);
                
                // 模拟读取耗时
                bbtco_sleep(100);
                
                reader_count--;
                printf("Reader %d: 完成读取，当前读者数量: %d\n", i, reader_count.load());
                
                // 锁会在作用域结束时自动释放
                comutex->UnLock();
                
                // 读取间隔
                bbtco_sleep(200);
            }
        };
    }
    
    // 创建多个写者协程
    for (int i = 0; i < 3; ++i) {
        bbtco_desc("writer") [&, i](){
            for (int j = 0; j < 5; ++j) {
                // 写者获取锁
                comutex->Lock();
                
                writer_count++;
                printf("Writer %d: 开始写入，当前写者数量: %d\n", i, writer_count.load());
                
                // 模拟写入操作
                int new_value = i * 100 + j;
                *shared_data = new_value;
                printf("Writer %d: 写入值 %d\n", i, new_value);
                
                // 模拟写入耗时
                bbtco_sleep(300);
                
                writer_count--;
                printf("Writer %d: 完成写入，当前写者数量: %d\n", i, writer_count.load());
                
                // 锁会在作用域结束时自动释放
                comutex->UnLock();
                
                // 写入间隔
                bbtco_sleep(500);
            }
        };
    }
    
    // 创建一个监控协程
    bbtco_desc("monitor") [&](){
        for (int i = 0; i < 20; ++i) {
            printf("=== 监控报告 ===\n");
            printf("当前读者数量: %d\n", reader_count.load());
            printf("当前写者数量: %d\n", writer_count.load());
            printf("当前数据值: %d\n", *shared_data);
            printf("===============\n");
            
            bbtco_sleep(1000);
        }
    };

    // bbtco_sleep(300000);
    sleep(100);
}

int main()
{
    printf("=== 基础读写者问题示例 ===\n");
    
    g_scheduler->Start();
    
    // 运行基础示例
    ConsumerProblems();
    
    // 等待一段时间
    g_scheduler->Stop();
    
    return 0;
}