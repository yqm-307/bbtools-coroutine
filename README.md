# bbtools-coroutine


## 简介

bbtools-coroutine 基于 boost.context 实现的有栈 C++ 协程库，Go 风格语法，仅支持 Linux 平台，性能极高。

bbtools-coroutine 有以下特点：

1、有栈协程，支持协程间的高效切换
2、丰富的协程同步原语（Chan、CoSelect、CoMutex、CoRWMutex、CoCond、CoPool 等）
3、支持协程池，方便管理和复用协程
4、类似 Go 语言的协程语法和使用体验
5、事件驱动 I/O，Poller 后端可替换
6、syscall hook：协程内 socket / sleep / poll / DNS 等阻塞调用转为协程等待，不阻塞调度线程；非协程上下文直通原生
7、高性能的无锁队列实现
8、支持协程间的 defer 语义和异常处理

## 一、安装

### 依赖

- boost.context

    ```shell
    sudo apt install libboost-all-dev
    ```

- bbtools-core（运行时依赖，需先安装）

    ```shell
    git clone https://github.com/yqm-307/bbtools-core.git
    cd bbtools-core
    cd shell
    sudo ./build.sh
    ```

### 构建与安装

```shell
git clone https://github.com/yqm-307/bbtools-coroutine.git
cd bbtools-coroutine
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cd shell && sudo ./install.sh    # 头文件装到 /usr/local/include，库装到 /usr/local/lib
```

CMake 开关（默认全部 `OFF`）：

| 开关 | 作用 |
|------|------|
| `CMAKE_BUILD_TYPE=Release` | 优化构建；不指定时为无优化构建（本库不强制 Release） |
| `NEED_TEST=ON` | 编译单元测试（CTest 37 个套件） |
| `NEED_BENCHMARK=ON` | 编译 `benchmark_test/`（含 `unified_stress`） |
| `NEED_EXAMPLE=ON` | 编译 `example/`（额外注册 `Test_real_clients`） |
| `NEED_DEBUG=ON` | 编译 `debug/` |
| `PROFILE=ON` | 启用 Profiler |
| `DEBUG_INFO=ON` | 输出库 debug 信息 |
| `STRINGENT_DEBUG=ON` | 严格 debug 模式，性能开销极大 |

> `build.sh` 等价于 `cmake -DRELEASE=ON -DNEED_EXAMPLE=ON ..` + `make` + `shell/install.sh`；其中 `-DRELEASE=ON` 当前未被 CMake 使用，构建类型以 `CMAKE_BUILD_TYPE` 为准。未安装 Ninja 时可去掉 `-G Ninja`。

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

Chan 是协程间通信的重要工具，支持阻塞读写：`Write`/`Read` 返回 0 成功、-1 失败；`TryWrite`/`TryRead` 立即返回，带毫秒参数的版本等待至超时；`Close()` 后读方以失败返回；`IsClosed()` 查询关闭状态：

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
#include <bbt/coroutine/sync/CoLockGuard.hpp>
using namespace bbt::coroutine;

void CoMutexExample()
{
    auto comutex = bbtco_make_comutex();
    int shared_data = 0;

    // 创建多个协程竞争访问共享资源
    for (int i = 0; i < 5; ++i) {
        bbtco [&, i](){
            // RAII 守卫：构造加锁、析构解锁（Lock() 返回 void，不能接返回值）
            sync::CoLockGuard<sync::CoMutex> lock(comutex);
            printf("coroutine %d got lock\n", i);
            
            // 模拟临界区操作
            int old_value = shared_data;
            bbtco_sleep(100);  // 模拟耗时操作
            shared_data = old_value + 1;
            
            printf("coroutine %d: %d -> %d\n", i, old_value, shared_data);
            // lock 在作用域结束时自动释放
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

    // 等待运行中的任务退出；队列中未执行的任务被取消式排空
    //（不执行；带future的任务以broken_promise兑现，见下）
    co_pool->Release();
    printf("All running tasks exited\n");
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

### 8. 多路复用（CoSelect）

CoSelect 提供 Go select 风格的多 Chan 复用，返回命中 Case 的注册序下标（-1 = 超时 / Default / 唤醒后均未就绪）：

```cpp
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoSelect.hpp>
using namespace bbt::coroutine;
using namespace bbt::coroutine::sync;

void CoSelectExample()
{
    auto ch1 = sync::Chan<int, 4>{};
    auto ch2 = sync::Chan<int, 4>{};

    // 先写入数据，Run 立即命中注册序最靠前的就绪 Case（这里 idx=0）
    ch1.TryWrite(1);

    int a = 0;
    int b = 0;
    int idx = CoSelect().CaseRead(ch1, a).CaseRead(ch2, b).CaseTimeout(1000).Run();
    printf("select idx=%d a=%d b=%d\n", idx, a, b);
}

int main()
{
    g_scheduler->Start();
    CoSelectExample();
    g_scheduler->Stop();
    return 0;
}
```

支持 `CaseRead` / `CaseWrite` / `CaseTimeout` / `Default`；只支持缓冲 Chan（`Chan<T, 0>` 编译期拦截）。

### 9. 读写锁（CoRWMutex）

CoRWMutex 提供协程读写锁：`RLock`/`WLock` 阻塞加锁，`TryRLock`/`TryWLock` 非阻塞或带毫秒超时，配合 RAII 守卫 `CoReadLock`/`CoWriteLock` 使用：

```cpp
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoLockGuard.hpp>
using namespace bbt::coroutine;
using namespace bbt::coroutine::sync;

void CoRWMutexExample()
{
    auto rwmutex = bbtco_make_corwmutex();

    // 读协程：共享读锁，可多个读者并存
    bbtco [&](){
        CoReadLock guard(rwmutex);
        printf("reader holds read lock\n");
        bbtco_sleep(100);
    };

    // 写协程：独占写锁
    bbtco [&](){
        CoWriteLock guard(rwmutex);
        printf("writer holds write lock\n");
    };

    sleep(1);
}

int main()
{
    g_scheduler->Start();
    CoRWMutexExample();
    g_scheduler->Stop();
    return 0;
}
```

### 10. Hook / I/O

协程内调用常见阻塞 syscall 时，Hook 将其转为协程等待，不占死调度线程；非协程上下文直通原生实现，第三方同步客户端（如 hiredis）可原样复用。

已拦截：`socket`、`connect`、`accept`/`accept4`、`send`/`sendto`/`sendmsg`、`recv`/`recvfrom`/`recvmsg`、`read`/`readv`、`write`/`writev`、`close`、`poll`/`select`/`pselect`、`sleep`/`usleep`/`nanosleep`/`clock_nanosleep`、`getaddrinfo`/`getnameinfo`。

- 传入 blocking FD 无需自行改造：协程 IO 期间临时 `O_NONBLOCK`，返回时恢复原 flags；
- `MSG_DONTWAIT` 直通原生；`SO_RCVTIMEO`/`SO_SNDTIMEO` 保持有界返回 `-1/EAGAIN`；
- 等待中的 FD 被 `close` 时唤醒等待协程并返回 `EBADF`，不会永久挂起；
- 语义边界与迁移注意见「三之二、停机与生命周期契约」。

调用矩阵与契约测试：`Test_hook_contract`、`Test_hook_blocking_fd`、`Test_hook_timeout_flags`、`Test_hook_error_matrix`。

## 三、API参考

### 基础API

| 宏/函数 | 描述 | 示例 |
|---------|------|------|
| `bbtco` | 创建协程 | `bbtco [](){}` |
| `bbtco_desc(name)` | 创建带描述的协程（描述可被诊断现场读回） | `bbtco_desc("worker") [](){}` |
| `bbtco_ref` | 创建引用捕获的协程 | `bbtco_ref {}` |
| `bbtco_noexcept(&succ)` | 注册协程，失败时置 `succ=false` 而不抛异常 | `bool succ; bbtco_noexcept(&succ) [](){};` |
| `bbtco_yield` | 协程让出 CPU | `bbtco_yield;` |
| `bbtco_sleep(ms)` | 协程睡眠指定毫秒 | `bbtco_sleep(1000);` |
| `bbtco_defer` | 延迟执行语句 | `bbtco_defer { cleanup(); };` |
| `GetLocalCoroutineId()` | 获取当前协程 ID | `auto id = GetLocalCoroutineId();` |

### 同步原语

| 类型 | 创建方式 | 主要方法 | 描述 |
|------|----------|----------|------|
| `Chan<T, Size>` | `sync::Chan<int, 100>{}` | `Write()`, `Read()`, `TryWrite()`, `TryRead()`, `Close()`, `IsClosed()`, `<<` / `>>` | 协程间通信通道（`Size=0` 为无缓冲，写阻塞至读端取走） |
| `CoSelect` | `sync::CoSelect()` | `CaseRead()`, `CaseWrite()`, `CaseTimeout()`, `Default()`, `Run()` | 多 Chan 复用，返回命中下标（-1 = 未命中） |
| `CoMutex` | `bbtco_make_comutex()` | `Lock()`, `UnLock()`, `TryLock()` | 协程互斥锁；RAII 用 `CoLockGuard<CoMutex>` |
| `CoRWMutex` | `bbtco_make_corwmutex()` | `RLock()`, `WLock()`, `RUnLock()`, `WUnLock()`, `TryRLock()`, `TryWLock()` | 协程读写锁；RAII 用 `CoReadLock` / `CoWriteLock` |
| `CoLockGuard<Mutex>` | `sync::CoLockGuard<CoMutex> g(lock)` | 构造加锁，析构解锁 | 通用 RAII 守卫（另有 `CoUniqueLock`） |
| `CoCond` | `bbtco_make_cocond()` | `Wait()`, `NotifyOne()`, `NotifyAll()` | 协程条件变量 |
| `CoPool` | `bbtco_make_copool(size)` | `Submit()`, `SubmitWithFuture()`, `Release()` | 协程池（`Release` 取消式停机：等运行中退出、排空未执行） |

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
| `g_scheduler->Start(opt)` | 启动调度器（默认后台线程模式） | 程序开始时调用 |
| `g_scheduler->Stop()` | 停止调度器（取消式停机，见下） | 程序结束时调用 |
| `g_scheduler->LoopOnce()` | 单次调度循环（用于手动驱动/测试） | 手动模式循环调用 |
| `g_scheduler->IsRunning()` | 调度器是否在运行 | 注册任务前检查 |

## 三之二、停机与生命周期契约（v1 M1）

与旧文档/旧行为的关键差异，迁移时必读：

| 变更 | 新契约 | 迁移动作 |
|------|--------|----------|
| `Scheduler::Stop()` | 取消式停机：不强杀运行中协程；parked（fd/定时器等待）协程被唤醒销毁；未执行任务真回收（不再泄漏）。Stop 有界返回，可重复调用 | 停机时刻不要依赖任务"跑完"；需要完成语义的，停机前自行等待业务 latch |
| Stop 后注册任务 | 明确失败：`bbtco_noexcept` 的 succ=false，无 noexcept 版抛异常（旧行为：Release 下假成功+泄漏） | 注册前检查 `IsRunning()` 或接住异常 |
| `CoPool::Release()` | 取消式：停止接收新任务、等待运行中协程退出、排空未执行任务；带 future 的被排任务以 `broken_promise` 兑现，不会永挂 | `SubmitAndWait` 的 future 增加 `broken_promise` catch；不要指望 Release 后 future 全部有效 |
| Hook IO 与 FD | 库不要求也不期望传入 blocking fd：协程 IO 期间临时强制 `O_NONBLOCK`，返回时恢复原 flags（#260）；`MSG_DONTWAIT` 直通原生（#261）；`SO_RCVTIMEO/SNDTIMEO` 由协程 deadline 实现有界返回 `-1/EAGAIN`（#261） | 多线程共享同一 fd 并发做 IO 的旧代码需自查 flags 竞态（契约排除项） |
| 等待中 fd 被 close | 唤醒等待协程，重试 syscall 返回 `EBADF`（#262，不再永久挂起） | 依赖"close 后等待者自醒"的代码语义已可正常工作 |
| detached 协程抛异常 | 保存 `exception_ptr` 可取回（#267）；无回调时日志+计数（`m_unhandled_exception_count`），不静默吞、不 terminate（#275） | 设置 `m_ext_coevent_exception_callback` 收口生产环境异常 |
| Release 构建栈释放 | `Stack::Clear/~Stack` 真正 free（#279，旧版 assert 吞副作用致每栈泄漏） | 无需动作；升级后 Release RSS 应下降 |
| 诊断现场 | `bbtco_desc` 的描述现在真正落库，协程内 `GetDescription()`/`GetWaitInfo()` 可读回（#276） | 给关键协程起名字 |
| worker 停顿告警 | 死循环/外部阻塞占住 worker 超 `m_cfg_worker_stall_warn_ms`（默认 0=关闭）时，调度线程上报 `WorkerStallInfo`（#277） | 生产建议设 500–1000ms 并接回调 |
| 栈溢出边界 | 默认开栈底保护页，溢出 = SIGSEGV fail-fast（#278） | 关保护页（`m_cfg_stack_protect=false`）前确认接受未定义行为 |
| 协作式取消 | `Coroutine::RequestCancel()` 置标志，协程在检查点/唤醒处自行退出（#266），不强杀 | 长循环协程定期检查 `IsCancelRequested()` |

## 三之三、测试、冒烟与压测

```shell
# 构建并运行全部单元测试
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure        # 37 个核心测试套件

# 冒烟（独立构建 + 报告）
python3 scripts/ci/run_smoke.py --build-dir build-ci-smoke

# 长时 soak（默认 6 小时；先构建 unified_stress）
cmake -S . -B build-soak -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_BENCHMARK=ON
cmake --build build-soak --target unified_stress --parallel
python3 scripts/ci/run_soak.py --build-dir build-soak
```

- 核心套件 37 个：协程状态机、调度与停机、取消、异常、hook 契约、同步原语、eventloop 契约、profiler 等；完整列表与 CI 说明见 `docs/ci-guide.md`。
- 真实客户端验收：`NEED_EXAMPLE=ON` 时额外注册 `Test_real_clients`（Echo + hiredis；本机无 Redis 时以 skip 77 跳过，不伪造通过）。
- 压测：`benchmark_test/unified_stress.cc` 提供六模块长时压测入口，由 `run_soak.py` 驱动。
- 报告目录：
  - `scripts/ci/run_smoke.py` → `tests/reports/smoke/<UTC 时间戳>/`（`summary.json`、`commands.json`、`ctest.xml`）
  - `scripts/ci/run_soak.py` → `tests/reports/soak/`
  - `tests/reports/` 与 `tests/ci-reports/` 下的原始日志/采样已被 `.gitignore` 忽略，不提交。

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
- 协程内异常不会传播到主线程：detached 协程保存 `exception_ptr` 可取回；无异常回调时记日志并计数，不静默吞、不 terminate
- 生产环境建议设置异常回调（`m_ext_coevent_exception_callback`）统一收口
- 使用 defer 进行资源清理

### 4. 性能优化
- 避免在协程池中提交大量长时间 CPU 计算任务（会占住 worker）
- 合理设置 Chan 的缓冲区大小
- 使用引用捕获避免不必要的拷贝

### 5. 调试技巧
- 使用 `bbtco_desc` 为协程添加描述信息（诊断现场可读回）
- 通过 `GetLocalCoroutineId()` 追踪协程执行
- 阻塞 syscall 会被 Hook 转成协程等待，无需刻意规避；纯 CPU 长任务会占住 worker，可开启 worker stall 告警（`m_cfg_worker_stall_warn_ms`）发现

## 五、性能对比，libgo，go

> 历史表格（三 CPU 执行 1000w 协程，2025-07 前口径）因基准源码已调整
> （`benchmark_coroutine.cc` 现为 100w 协程）且无复现记录，已移除。
> 以下为当前源码可复现的实测数据。

**bbtco 协程调度基准（100w 协程，`benchmark_test/benchmark_coroutine.cc`，nsum_co=1000000）：**

| 日期 | 机器 | 编译 | 耗时 |
|------|------|------|------|
| 2026-09-08 | ubuntu-persion, i7-13790F 4核, g++ 13.3, Release, main@63de06a | `ninja benchmark_coroutine` | 1229 / 1282 / 1354 ms（3 次） |

**CoPool 1000w 任务提交（`benchmark_test/benchmark_copool.cc`，nsum_co=10000000）：**

| 日期 | 机器 | 编译 | 耗时 |
|------|------|------|------|
| 2026-09-08 | 同上 | `ninja benchmark_copool` | 1468 ms |

复现命令：`cd build && cmake .. -G Ninja -DNEED_BENCHMARK=ON && ninja && ./bin/benchmark_test/benchmark_coroutine`。CPU/内存频率不同会带来误差，仅供参考；bbtco 内部使用无锁队列，CPU/内存频率越高调度效率越高。原始报告：`tests/ci-reports/`（runner 本地，不提交）。

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

