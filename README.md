# bbtools-coroutine


## 简介

bbtools-coroutine基于 boost.context 实现的go 风格C++协程方案，仅支持linux平台。性能极高。

bbtools-coroutine有以下特点：

1、实现了有栈协程，支持协程间的高效切换
2、提供了丰富的协程同步原语（Chan、CoMutex、CoCond等）
3、支持协程池，方便管理和复用协程
4、提供了类似Go语言的协程语法和使用体验
5、基于事件驱动的异步I/O支持
6、高性能的无锁队列实现
7、支持协程间的defer语义和异常处理

## 一、安装
- boost.context

    ```shell
    sudo apt install libboost-all-dev
    ```

- bbtools-core

    ```shell
    git clone https://github.com/yqm-307/bbtools-core.git
    cd bbtools-core
    cd shell
    sudo ./build.sh
    ```
    
- libevent

    ```shell
    sudo apt install libevent-dev
    ```

- bbtools_coroutine

    ```shell
    git clone ${库地址}
    cd ${库文件夹}
    sudo ./build.sh
    ```

## 二、基础使用

### 1. 协程基础语法

首先以后台线程模式启动调度器，注册协程并运行：

```cpp
#include <bbt/coroutine/coroutine.hpp>
using namespace bbt::coroutine;

int main()
{
    // 启动调度器
    g_scheduler->Start();

    // 创建协程 - 基础语法
    bbtco [](){
        printf("i am coroutine %lu\n", GetLocalCoroutineId());
        bbtco_yield;  // 让出CPU控制权
        printf("i am coroutine %lu after yield\n", GetLocalCoroutineId());
    };

    // 创建协程 - 带描述
    bbtco_desc("worker") [](){
        printf("i am worker coroutine\n");
        bbtco_sleep(100);  // 协程睡眠100ms
        printf("worker coroutine wake up\n");
    };

    sleep(1);
    
    // 停止调度器
    g_scheduler->Stop();
    return 0;
}
```

### 2. 协程通道（Chan）

Chan是协程间通信的重要工具，支持阻塞读写操作：

```cpp
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

void ChanExample()
{
    // 创建一个容量为1的整型通道
    auto chan = sync::Chan<int, 1>{};

    // 写入协程
    bbtco [&](){
        printf("writing to chan...\n");
        chan << 42;  // 写入数据
        printf("write success\n");
    };

    // 读取协程
    bbtco [&](){
        int value = 0;
        printf("reading from chan...\n");
        chan >> value;  // 读取数据
        printf("read value: %d\n", value);
    };

    sleep(1);
}

// 多写者示例
void MultiWriterExample()
{
    auto chan = sync::Chan<int, 10>{};

    // 创建多个写者协程
    for (int i = 0; i < 5; ++i) {
        bbtco [&, i](){
            chan << i;
            printf("writer %d wrote %d\n", i, i);
        };
    }

    // 创建读者协程
    bbtco [&](){
        for (int i = 0; i < 5; ++i) {
            int value = 0;
            chan >> value;
            printf("read value: %d\n", value);
        }
    };

    sleep(1);
}

int main()
{
    g_scheduler->Start();
    
    printf("=== Chan Example ===\n");
    ChanExample();
    
    printf("=== Multi Writer Example ===\n");
    MultiWriterExample();
    
    g_scheduler->Stop();
    return 0;
}
```

### 3. 协程条件变量（CoCond）

CoCond提供了协程间的等待和唤醒机制：

```cpp
#include <bbt/coroutine/coroutine.hpp>
using namespace bbt::coroutine;

void CoCondExample()
{
    auto cocond = bbtco_make_cocond();

    // 等待者协程
    bbtco [&](){
        printf("coroutine %lu waiting...\n", GetLocalCoroutineId());
        cocond->Wait();  // 挂起当前协程
        printf("coroutine %lu awakened!\n", GetLocalCoroutineId());
    };

    // 唤醒者协程
    bbtco [&](){
        bbtco_sleep(500);  // 等待500ms
        printf("notify one coroutine\n");
        cocond->NotifyOne();  // 唤醒一个等待的协程
    };

    sleep(1);
}

void NotifyAllExample()
{
    auto cocond = bbtco_make_cocond();

    // 创建多个等待者
    for (int i = 0; i < 3; ++i) {
        bbtco [&, i](){
            printf("coroutine %d waiting...\n", i);
            cocond->Wait();
            printf("coroutine %d awakened!\n", i);
        };
    }

    // 唤醒所有等待者
    bbtco [&](){
        bbtco_sleep(500);
        printf("notify all coroutines\n");
        cocond->NotifyAll();  // 唤醒所有等待的协程
    };

    sleep(1);
}

int main()
{
    g_scheduler->Start();
    
    printf("=== CoCond Example ===\n");
    CoCondExample();
    
    printf("=== NotifyAll Example ===\n");
    NotifyAllExample();
    
    g_scheduler->Stop();
    return 0;
}
```

### 4. 协程互斥锁（CoMutex）

CoMutex提供了协程间的互斥访问：

```cpp
#include <bbt/coroutine/coroutine.hpp>
using namespace bbt::coroutine;

void CoMutexExample()
{
    auto comutex = bbtco_make_comutex();
    int shared_data = 0;

    // 创建多个协程竞争访问共享资源
    for (int i = 0; i < 5; ++i) {
        bbtco [&, i](){
            auto lock = comutex->Lock();  // 获取锁
            printf("coroutine %d got lock\n", i);
            
            // 模拟临界区操作
            int old_value = shared_data;
            bbtco_sleep(100);  // 模拟耗时操作
            shared_data = old_value + 1;
            
            printf("coroutine %d: %d -> %d\n", i, old_value, shared_data);
            // lock在作用域结束时自动释放
        };
    }

    sleep(1);
    printf("final shared_data: %d\n", shared_data);
}

int main()
{
    g_scheduler->Start();
    
    printf("=== CoMutex Example ===\n");
    CoMutexExample();
    
    g_scheduler->Stop();
    return 0;
}
```

### 5. 协程池（CoPool）

CoPool提供了协程的池化管理：

```cpp
#include <bbt/coroutine/coroutine.hpp>
using namespace bbt::coroutine;

void CoPoolExample()
{
    // 创建一个最大协程数为2的协程池
    auto co_pool = bbtco_make_copool(2);

    // 提交任务到协程池
    co_pool->Submit([]() {
        bbtco_sleep(500);
        printf("Task 1 completed at %s\n", bbt::core::clock::getnow_str().c_str());
    });

    // 提交多个任务
    for (int i = 0; i < 4; ++i) {
        co_pool->Submit([i]() {
            bbtco_sleep(300);
            printf("Task %d completed at %s\n", i + 2, bbt::core::clock::getnow_str().c_str());
        });
    }

    // 等待所有任务完成
    co_pool->Release();
    printf("All tasks completed\n");
}

int main()
{
    g_scheduler->Start();
    
    printf("=== CoPool Example ===\n");
    CoPoolExample();
    
    g_scheduler->Stop();
    return 0;
}
```

### 6. 事件等待（WaitFor）

支持基于文件描述符的异步I/O操作：

```cpp
#include <bbt/coroutine/coroutine.hpp>
#include <unistd.h>
using namespace bbt::coroutine;

void WaitForExample()
{
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        return;
    }

    // 读取协程
    bbtco [&](){
        printf("waiting for data on fd %d\n", pipefd[0]);
        
        // 等待文件描述符可读
        bbtco_wait_for(pipefd[0], bbtco_emev_readable, 0);
        
        char buffer[64] = {0};
        read(pipefd[0], buffer, sizeof(buffer));
        printf("received: %s\n", buffer);
    };

    // 写入协程
    bbtco [&](){
        bbtco_sleep(1000);  // 等待1秒
        printf("writing data to fd %d\n", pipefd[1]);
        write(pipefd[1], "Hello from pipe!", 16);
    };

    sleep(2);
    close(pipefd[0]);
    close(pipefd[1]);
}

void TimeoutExample()
{
    bbtco [](){
        printf("timeout test start\n");
        bbtco_wait_for(0, bbtco_emev_timeout, 500);  // 等待500ms超时
        printf("timeout after 500ms\n");
    };

    sleep(1);
}

int main()
{
    g_scheduler->Start();
    
    printf("=== WaitFor Example ===\n");
    WaitForExample();
    
    printf("=== Timeout Example ===\n");
    TimeoutExample();
    
    g_scheduler->Stop();
    return 0;
}
```

### 7. Defer语义

支持类似Go语言的defer语义：

```cpp
#include <bbt/coroutine/coroutine.hpp>
using namespace bbt::coroutine;

void DeferExample()
{
    bbtco [](){
        printf("coroutine start\n");
        
        // defer按照逆序执行
        bbtco_defer { printf("defer 1\n"); };
        bbtco_defer { printf("defer 2\n"); };
        bbtco_defer { printf("defer 3\n"); };
        
        printf("coroutine body\n");
        
        // 协程结束时，defer按照逆序执行：defer 3, defer 2, defer 1
    };

    sleep(1);
}

void DeferWithResourceExample()
{
    bbtco [](){
        // 使用defer进行资源管理
        FILE* file = fopen("test.txt", "w");
        if (file) {
            bbtco_defer { 
                fclose(file); 
                printf("file closed\n");
            };
            
            fprintf(file, "Hello World\n");
            printf("file written\n");
        }
        
        // 文件会在协程结束时自动关闭
    };

    sleep(1);
}

int main()
{
    g_scheduler->Start();
    
    printf("=== Defer Example ===\n");
    DeferExample();
    
    printf("=== Defer Resource Example ===\n");
    DeferWithResourceExample();
    
    g_scheduler->Stop();
    return 0;
}
```

## 三、API参考

### 基础API

| 宏/函数 | 描述 | 示例 |
|---------|------|------|
| `bbtco` | 创建协程 | `bbtco [](){}` |
| `bbtco_desc(name)` | 创建带描述的协程 | `bbtco_desc("worker") [](){}` |
| `bbtco_ref` | 创建引用捕获的协程 | `bbtco_ref {}` |
| `bbtco_yield` | 协程让出CPU | `bbtco_yield;` |
| `bbtco_sleep(ms)` | 协程睡眠ms毫秒 | `bbtco_sleep(1000);` |
| `bbtco_defer` | 延迟执行语句 | `bbtco_defer { cleanup(); };` |
| `GetLocalCoroutineId()` | 获取当前协程ID | `auto id = GetLocalCoroutineId();` |

### 同步原语

| 类型 | 创建方式 | 主要方法 | 描述 |
|------|----------|----------|------|
| `Chan<T, Size>` | `sync::Chan<int, 100>{}` | `Write()`, `Read()`, `Close()` | 协程间通信通道 |
| `CoMutex` | `bbtco_make_comutex()` | `Lock()`, `UnLock()` | 协程互斥锁 |
| `CoCond` | `bbtco_make_cocond()` | `Wait()`, `NotifyOne()`, `NotifyAll()` | 协程条件变量 |
| `CoPool` | `bbtco_make_copool(size)` | `Submit()`, `Release()` | 协程池 |

### 事件等待

| 宏 | 描述 | 示例 |
|----|------|------|
| `bbtco_wait_for(fd, event, timeout)` | 等待文件描述符事件 | `bbtco_wait_for(fd, bbtco_emev_readable, 1000)` |
| `bbtco_emev_readable` | 可读事件 | 等待fd可读 |
| `bbtco_emev_writeable` | 可写事件 | 等待fd可写 |
| `bbtco_emev_timeout` | 超时事件 | 纯超时等待 |

### 调度器控制

| 方法 | 描述 | 示例 |
|------|------|------|
| `g_scheduler->Start()` | 启动调度器 | 程序开始时调用 |
| `g_scheduler->Stop()` | 停止调度器 | 程序结束时调用 |

## 四、注意事项

### 1. 协程生命周期管理
- 协程创建后立即被调度执行
- 协程函数结束时自动销毁
- 避免协程函数中使用栈上的引用，除非能确保生命周期

### 2. 内存管理
- 协程栈大小可配置，默认2MB
- 避免在协程中分配大量栈空间
- 使用智能指针管理堆内存

### 3. 异常处理
- 协程内的异常不会传播到主线程
- 建议在协程内部处理异常
- 使用defer进行资源清理

### 4. 性能优化
- 避免在协程池中提交大量阻塞任务
- 合理设置Chan的缓冲区大小
- 使用引用捕获避免不必要的拷贝

### 5. 调试技巧
- 使用`bbtco_desc`为协程添加描述信息
- 通过`GetLocalCoroutineId()`追踪协程执行
- 避免在协程中使用阻塞的系统调用

## 五、性能对比，libgo，go

这里看下不同频率的cpu下，执行1000w协程的耗时。这里内存频率也不同，会导致误差，仅供参考。不过因为bbtco内使用了无锁队列，所以cpu频率越高、内存频率越高，框架整体协程调度效率越高。

|       | Intel(R) Xeon(R) CPU E5-26xx v4（2.1Ghz） | Intel(R) Core(TM) Ultra 7 155H | 13th Gen Intel(R) Core(TM) i7-13790F |
| ----- | ----------------------------------------- | ------------------------------ | ------------------------------------ |
| libgo | N/A ms(coredump)                          | N/A ms                        | 25261 ms                              |
| bbtco | 13763 ms                                  | 15101 ms                        | 5699 ms                              |
| go    | 3664 ms                                   | 6921 ms                         | 4250 ms                               |

## 六、综合示例

下面是一个综合示例，展示了生产者-消费者模式的完整实现：

```cpp
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
```

这个综合示例展示了：
- 多个生产者协程并发产生任务
- 多个消费者协程并发消费任务
- 使用协程池处理计算密集型任务
- 使用Chan进行协程间通信
- 使用defer确保资源清理
- 使用原子变量进行状态统计
- 协程的生命周期管理

