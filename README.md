# bbtools-coroutine

## 简介

bbtools-coroutine 是基于 boost.context 的 Linux 平台有栈协程运行时，提供 scheduler、Hook、sync 原语、CoPool 和一套 Go 风格语法糖。

本阶段收口后的对外契约重点不是“更多能力”，而是以下稳定前提：

1. 所有依赖协程调度的 public API 都要求先启动 `g_scheduler->Start()`。
2. `detail::GlobalConfig` 里的线程数、栈大小、扫描间隔等配置应在第一次 `Start()` 之前完成。
3. `bbtco_sleep`、`bbtco_wait_for` 等 suspend 类 helper 只能在协程上下文中使用。
4. `GetLocalCoroutineId()`、`GetLocalCoroutineStackSize()` 在非协程上下文中返回 0。
5. `g_scheduler->Stop()` 只能从非 worker 上下文调用；在协程 worker 里调用会抛出 `std::runtime_error`。
6. `bbtco` / `bbtco_desc` 在 scheduler 未运行时沿用注册失败异常路径；`bbtco_noexcept` 则把结果写入 `succ`。
7. 开启 `-DCO_TRACE=ON` 后，可按 `CoroutineId`、描述信息或描述前缀追踪单个 coroutine 的最近调度轨迹。

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
    ## 二、稳定使用方式

    ### 1. 启动顺序

    最小安全顺序如下：

    ```cpp
    #include <bbt/coroutine/coroutine.hpp>
    using namespace bbt::coroutine;

    int main()
    {
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = 2;
        cfg->m_cfg_stack_size = 4096;

        g_scheduler->Start();

        bool succ = false;
        bbtco_noexcept(&succ) []() {
            printf("co=%lu stack=%zu\n", GetLocalCoroutineId(), GetLocalCoroutineStackSize());
            bbtco_yield;
            bbtco_sleep(50);
        };

        printf("register succ=%d\n", succ ? 1 : 0);
        sleep(1);
        g_scheduler->Stop();
        return 0;
    }
    ```

    ### 2. 线程上下文与 failure mode

    - `bbtco` / `bbtco_desc`：要求 scheduler 已运行；未运行时会沿用注册异常路径。
    - `bbtco_noexcept`：要求 scheduler 已运行；未运行时不会抛异常，而是把 `succ` 写成 `false`。
    - `bbtco_yield`：只在协程上下文里真正让出执行权；在非协程上下文里是 no-op。
    - `bbtco_sleep`：只能在协程上下文里调用；非协程上下文会命中断言。
    - `bbtco_wait_for`：只能在协程上下文里调用；底层等待失败时会抛出 `std::runtime_error`。
    - `GetLocalCoroutineId()` / `GetLocalCoroutineStackSize()`：在非协程上下文中返回 `0`。
    - `SetCoroutineTraceFilter()` / `QueryCoroutineTrace()`：仅在 `CO_TRACE` 构建中产生详细 trace 数据；未命中过滤器的协程不会进入完整事件记录路径。
    - `g_scheduler->Stop()`：只能从非 worker 上下文调用；在协程 worker 中调用会抛出 `std::runtime_error`。

    ### 4. coroutine trace 调试

    需要在配置时显式开启：

    ```shell
    cmake -B build -DCO_TRACE=ON -DNEED_TEST=ON .
    cmake --build build
    ```

    基本用法：

    ```cpp
    CoroutineTraceFilter filter;
    filter.desc_prefixes.push_back("worker.");
    SetCoroutineTraceFilter(filter);

    g_scheduler->Start();

    detail::CoroutineId target_id = 0;
    bbtco_desc("worker.fetch-user") [&]() {
        target_id = GetLocalCoroutineId();
        bbtco_yield;
        bbtco_sleep(10);
    };

    // 之后可按 id 或 desc 查询
    auto snapshot = QueryCoroutineTrace(target_id);
    auto actives = QueryActiveCoroutinesByDesc("worker.", true);
    std::string dump = DumpCoroutineTrace(target_id);
    ```

    `PROFILE` 仍负责聚合统计，`DEBUG_INFO` 负责调试打印，`STRINGENT_DEBUG` 负责强约束断言；`CO_TRACE` 只负责单 coroutine 最近事件追踪。

    ### 3. 常见入口

    #### 基础协程

    ```cpp
    bbtco []() {
        printf("co=%lu\n", GetLocalCoroutineId());
        bbtco_yield;
        bbtco_sleep(100);
    };
    ```

    #### Chan / CoCond / CoMutex

    ```cpp
    auto chan = bbt::co::Chan<int, 1>();
    auto cond = bbtco_make_cocond();
    auto mutex = bbtco_make_comutex();

    bbtco [chan]() {
        *chan << 42;
    };

    bbtco [chan, cond, mutex]() {
        int value = 0;
        *chan >> value;
        auto guard = mutex->Lock();
        cond->NotifyOne();
        printf("value=%d\n", value);
    };
    ```

    #### wait_for 与事件 helper

    ```cpp
    int pipefd[2];
    pipe(pipefd);

    bbtco [=]() {
        bbtco_wait_for(pipefd[0], bbtco_emev_readable, 0);
        char ch = 0;
        ::read(pipefd[0], &ch, 1);
    };

    bbtco_ev_t(100) [](int, short) {
        printf("timeout fired\n");
    };
    ```

    #### CoPool

    ```cpp
    auto pool = bbtco_make_copool(2);
    pool->Submit([]() {
        bbtco_sleep(10);
    });
    pool->Release();
    ```
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
| `bbtco_sleep(ms)` | 在协程上下文中挂起当前协程 | `bbtco_sleep(1000);` |
| `bbtco_defer` | 延迟执行语句 | `bbtco_defer { cleanup(); };` |
| `GetLocalCoroutineId()` | 获取当前协程ID | `auto id = GetLocalCoroutineId();` |
| `SetCoroutineTraceFilter(filter)` | 设置 trace 过滤器 | `SetCoroutineTraceFilter(filter);` |
| `QueryCoroutineTrace(id)` | 查询单个协程最近事件 | `auto snapshot = QueryCoroutineTrace(id);` |
| `DumpCoroutineTrace(id)` | 输出单个协程的调试文本 | `std::cout << DumpCoroutineTrace(id);` |

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
| `g_scheduler->Start()` | 启动调度器，前置配置应已完成 | 程序开始时调用 |
| `g_scheduler->Stop()` | 从非 worker 上下文停止调度器 | 程序结束时调用 |

## 四、注意事项

### 1. 协程生命周期管理
- 协程注册成功后会被异步调度，不保证执行顺序与固定线程
- 协程函数结束时自动进入终态
- 避免协程函数中捕获生命周期短于协程任务本身的引用

### 2. 内存管理
- 协程栈大小可配置，默认2MB
- 避免在协程中分配大量栈空间
- 使用智能指针管理堆内存

### 3. 异常处理
- 协程内的异常不会传播到主线程
- 建议在协程内部处理异常
- 使用 `bbtco_defer` 进行资源清理

### 4. 性能优化
- 避免在协程池中提交大量阻塞任务
- 合理设置Chan的缓冲区大小
- 使用引用捕获避免不必要的拷贝

### 5. 调试技巧
- 使用`bbtco_desc`为协程添加描述信息
- 通过`GetLocalCoroutineId()`追踪协程执行
- 需要单协程取证时，用 `-DCO_TRACE=ON` 配合 `SetCoroutineTraceFilter` / `QueryCoroutineTrace`
- suspend 类 helper 只在协程上下文中调用

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
