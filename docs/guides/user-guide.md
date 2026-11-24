# bbtools-coroutine 使用指南

本文面向调用 `bbtools-coroutine` 的应用、adapter 和其他仓库维护者。目标是帮助你按场景选对入口，并避开协程库最容易出现的生命周期、线程和等待错误。

本文描述当前实现的推荐用法。完整签名、返回码和前置条件以 [API 参考](api-reference.md) 和对应头文件为准。运行时进程寿命、线程模型、Hook 边界和网络执行域以[核心运行时契约](../architecture/core-runtime-contract.md)为准。

## 先记住 8 条规则

1. 应用入口优先使用 `BBTCO_MAIN*`，不要在常规应用中自行启动调度器线程。
2. `bbtco` 注册的是 detached 协程，不保证注册顺序，也不提供 `join`。
3. 协程只能在受管协程上下文中使用会挂起的 API，例如 `Chan` 阻塞读写、`CoMutex::Lock`、`CoCond::Wait` 和 `bbtco_wait_for`。
4. 协程挂起后可能换到其他 worker 恢复。不要把 worker 线程、线程局部对象或线程内锁的归属带过挂起点。
5. 跨越协程生命周期或延迟执行边界的数据必须有明确所有权。无法证明外部对象活得足够久时，使用值捕获、移动或 `shared_ptr`。
6. 协程内的 `sleep`、Hook I/O 和同步原语只挂起当前协程，不应改写成 native 线程等待。
7. 顺序 async 函数用 `bbt_co_await` 组合操作，用 `bbt_co_return` 包装最终结果。需要延迟执行的操作使用 `bbt_co_bind`。
8. 业务完成必须在主任务返回前收口。runtime 是进程寿命对象，没有业务停机入口，也不会替应用等待 detached 协程完成。

## 1. 入口、依赖和最小程序

### 1.1 环境

- Linux。
- C++17。
- Boost 1.90 或更高版本，CMake 需要 `boost.context`。
- 公共入口是 `#include <bbt/coroutine/coroutine.hpp>`。
- 应用需要链接 `bbt_coroutine`。

公共伞头已经包含 runtime、语法宏、Hook、同步原语、协程池和协程对象的常用入口。只在需要缩小依赖或阅读具体接口时直接包含子模块头文件。

### 1.2 推荐的最小入口

常规应用定义 `int bbtco_main()`，由 `BBTCO_MAIN()` 生成 native `main`。主任务本身运行在受管协程中，runtime 会等待主任务完成并返回其退出结果。

```cpp
#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

int bbtco_main()
{
    bbtco_desc("worker") []() {
        bbtco_sleep(10);
    };

    bbtco_sleep(20);
    return 0;
}

BBTCO_MAIN()
```

`bbtco_desc` 只用于给诊断现场增加可读名称。协程注册成功后，执行时间和 worker 都不固定。

常规应用不要自行调用 `g_scheduler->Start()`、自建 runtime runner 线程或在主线程 sleep 等待任务。需要调整 worker 数、默认栈或栈预算时，使用 `BBTCO_MAIN_WITH_OPTIONS(...)`。需要在 `Start` 前修改完整 `GlobalConfig` 时，使用 `BBTCO_MAIN_WITH_SETUP()`。入口宏的完整集合见 `bbt/coroutine/syntax/RuntimeMacros.hpp`。

### 1.3 入口宏怎么选

| 需求 | 推荐入口 |
|---|---|
| 默认配置启动应用 | `BBTCO_MAIN()` |
| 启动前设置 worker 数、默认栈或栈预算 | `BBTCO_MAIN_WITH_OPTIONS(options_expr)` |
| 启动前执行 `setup_runtime()` 修改全局配置 | `BBTCO_MAIN_WITH_SETUP()` |
| 同时需要 setup 和 `RuntimeOptions` | `BBTCO_MAIN_WITH_SETUP_AND_OPTIONS(options_expr)` |
| 需要提交前 native hook | 对应的 `*_WITH_HOOK` 入口 |
| 主任务提交后需要一次 native host hook | `BBTCO_MAIN_WITH_SETUP_AND_HOST_HOOK*` |

`RuntimeOptions` 只覆盖 `initial_workers`、`default_stack` 和 `stacks`。所有配置必须在 runtime 启动前确定，启动后不要修改底层 `GlobalConfig`。

### 1.4 主任务完成和进程退出

`RuntimeMain` 等待 `bbtco_main` 返回。它不会停止 scheduler，也不会排空其他 detached 协程。需要保证任务完成时，在主任务内使用协程原语收齐工作，再返回。

适合收口的方式包括：

- 使用 `Chan` 发送每个 worker 的完成结果。
- 使用 `CoCond` 配合共享计数和谓词。
- 使用 `CoPool::Release()` 等待池内任务退出。
- 使用应用自己的协程友好计数器或 latch。

不要用 native `std::condition_variable`、`std::future::wait()`、`std::future::get()` 或线程 `join()` 代替协程等待。它们会占住执行它们的 worker 或 native 线程。

## 2. 执行模型

bbtools-coroutine 是有栈协程库。scheduler 管理多个 Processer，Processer 通常对应一个 worker 线程，协程代码运行在独立栈上。

```text
应用代码
  ├─ bbtco 注册任务
  ├─ Chan / CoMutex / CoCond 等等待
  └─ 协程内 Hook I/O
          │
          ▼
      Scheduler
          │
          ▼
   Processer × N
          │
          ▼
  Coroutine 独立栈
          │ 挂起
          ▼
  Poller / waiter / timer
          │ 唤醒
          └────重新进入就绪队列
```

需要据此理解几个行为：

- 多个 worker 可以并行执行不同协程。
- 同一协程同一时刻只在一个 worker 上运行。
- 协程挂起后允许在另一个 worker 上恢复。
- runtime 是协作式调度。没有抢占，用户态死循环或长时间 CPU 计算会占住当前 worker。
- 取消是协作式的。协程对象的 `RequestCancel()` 只设置标志，业务代码在检查点或等待返回后自行退出。
- 没有业务 `Stop()`。进程退出时，等待中的协程不会做栈展开式收口。

## 3. 创建协程、让出和延时

### 3.1 注册 detached 协程

```cpp
bbtco []() {
    // 运行在受管协程中
};

bbtco_desc("read-loop") [=]() {
    // 带描述名称的协程
};
```

lambda 会被保存到协程对象中，协程结束后释放。引用捕获仍然只是借用外部对象，不能因为 lambda 被保存就延长引用对象的寿命。

优先使用值捕获或移动捕获保存跨越调用边界的数据：

```cpp
std::string request = BuildRequest();
auto owner = std::make_shared<State>();

bbtco [request = std::move(request), owner]() {
    Send(request, owner);
};
```

`bbtco_ref { ... };` 等价于引用捕获块，只在外部对象明确活过整个协程时使用。常见的安全场景是协程不会逃出当前受控生命周期，且调用方有明确的收口顺序。无法证明时不要使用它。

注册失败默认在注册点抛异常。需要显式处理失败时使用：

```cpp
bool accepted = false;
bbtco_noexcept(&accepted) [=]() {
    // 只有 accepted 为 true 时才会被注册并执行
};
```

### 3.2 `bbtco_yield`

`bbtco_yield` 立即把当前协程放回就绪队列，之后可能在其他 worker 恢复。

```cpp
bbtco []() {
    UpdateSmallStep();
    bbtco_yield;
    ContinueSmallStep();
};
```

它适合把长的协作式计算拆成小步。它不提供顺序保证，也不能修复无界 CPU 循环。宏自带语句结束符，在单行 `if` 中使用时写花括号。

### 3.3 `bbtco_sleep(ms)`

协程内的 `bbtco_sleep(ms)` 挂起当前协程指定毫秒，不占住 worker。`ms <= 0` 不产生有效等待。

```cpp
bbtco []() {
    while (Running()) {
        PollApplicationState();
        bbtco_sleep(100);
    }
};
```

协程外调用会走 native sleep 分支。业务代码应保持调用域清晰，在协程内使用协程等待，在 native 控制面使用普通线程 API。

### 3.4 查询执行上下文

```cpp
const auto id = GetLocalCoroutineId();
const auto stack_size = GetLocalCoroutineStackSize();
const bool in_coroutine = IsInCoroutineContext();
```

非协程上下文分别返回 `0`、`0` 和 `false`。这些查询不创建 runtime，也不惰性启动 worker。只有确实要求协程上下文的入口才使用 `RequireCoroutineContext()`。

## 4. 等待和事件

### 4.1 等待文件描述符

```cpp
bbtco []() {
    bbtco_wait_for(fd, bbtco_emev_readable, 1000);
    ReadAvailableBytes(fd);
};
```

`bbtco_wait_for` 是语句，不返回状态码。当前协程会等待可读、可写、关闭或超时事件。等待失败抛出 `std::runtime_error`，需要在业务边界统一捕获。

可用事件包括：

- `bbtco_emev_readable`
- `bbtco_emev_writeable`
- `bbtco_emev_close`
- `bbtco_emev_timeout`

纯定时器使用 `bbtco_wait_for(0, bbtco_emev_timeout, ms)`。需要「事件触发后再创建协程」时使用 `bbtco_ev_r`、`bbtco_ev_w`、`bbtco_ev_t` 及其 timeout / close 组合。事件回调签名是 `void(int fd, short event)`，回调在新协程中执行。

事件注册是一次性的。fd 的所有权仍由调用方负责，回调执行前必须保证 fd 仍然有效。

### 4.2 Hook I/O

协程内可以直接调用常见 POSIX I/O。Hook 会把阻塞路径转换为当前协程等待，避免占住 worker。当前覆盖范围包括 socket、connect、accept、send、recv、read、write、poll、select、sleep 和常见 DNS 函数。

推荐写法是直接使用已有系统调用：

```cpp
bbtco []() {
    const auto accepted = accept(listener, nullptr, nullptr);
    if (accepted < 0)
        return;

    bbtco_defer { close(accepted); };
    char buffer[4096];
    const auto size = read(accepted, buffer, sizeof(buffer));
    if (size > 0)
        write(accepted, buffer, static_cast<size_t>(size));
};
```

注意以下边界：

- 不要为了接入协程 Hook 手动把 fd 改成 `O_NONBLOCK`。库会在协程 I/O 期间临时处理 blocking fd，并在返回时恢复 flags。
- `MSG_DONTWAIT` 仍保持立即返回语义。
- 同一个 fd 不要由多个线程并发执行 Hook 路径 I/O。
- 等待中的 fd 被关闭时，等待协程会被唤醒，系统调用通常以 `EBADF` 返回。
- 第三方客户端是否安全，取决于它是否遵守 fd、线程和回调的自身约束。协程 Hook 只改变等待方式，不改变第三方库的线程安全契约。

## 5. 同步原语怎么选

| 场景 | 推荐能力 | 关键语义 |
|---|---|---|
| 传递任务或结果 | `Chan<T, N>` | 空读或满写时挂起协程 |
| 同时等待多个通道 | `CoSelect` | 只支持 `N > 0` 的缓冲 Chan |
| 保护短临界区 | `CoMutex` + `CoLockGuard` | 等待锁时挂起协程 |
| 读多写少 | `CoRWMutex` + `CoReadLock` / `CoWriteLock` | 写者优先 |
| 等待共享状态变化 | `CoCond` + `CoMutex` | Notify 不携带谓词，等待方必须复查 |
| 固定并发度执行任务 | `CoPool` | 固定数量池协程，任务队列与池生命周期分离 |

等待型 API 必须在受管协程内调用。锁和条件变量的对象必须比所有等待者活得久，销毁前先保证没有等待者。

### 5.1 Chan 生产者消费者

`Chan<T, N>` 的元素按值入队。`N > 0` 是有缓冲 Chan，`N == 0` 是无缓冲特化。读写端可以使用显式函数获得返回码，也可以使用 `>>` / `<<` 做简洁调用。

```cpp
using namespace bbt::coroutine;

auto tasks = Chan<Task, 16>();

bbtco [tasks]() {
    for (int i = 0; i < 100; ++i) {
        if (tasks->Write(Task{i}) != 0)
            return;
    }
    tasks->Close();
};

bbtco [tasks]() {
    Task task;
    while (tasks->Read(task) == 0)
        Process(task);
};
```

常用返回码如下：

- `0` 表示成功。
- `1` 表示带超时操作超时。
- `-1` 表示关闭或关闭后无数据，具体取决于调用的读写 API。
- `-2` 表示当前操作未完成或等待失败，具体取决于 API。

`Close()` 会唤醒等待者。有缓冲 Chan 会保留已入队数据，读者可以继续读完。生产者完成后关闭 Chan，消费者根据返回码退出，是最简单的完成传播方式。

不要用 `Chan<T, 0>` 配合 `CoSelect`。`CoSelect` 只支持缓冲 Chan。

### 5.2 CoSelect 多路等待

```cpp
using namespace bbt::coroutine;

auto left_channel = Chan<int, 4>();
auto right_channel = Chan<int, 4>();
int left = 0;
int right = 0;
const int selected = sync::CoSelect()
                         .CaseRead(left_channel, left)
                         .CaseRead(right_channel, right)
                         .CaseTimeout(1000)
                         .Run();

if (selected == 0) {
    Handle(left);
} else if (selected == 1) {
    Handle(right);
} else {
    HandleTimeout();
}
```

返回值是 case 注册顺序下标。`-1` 表示超时、命中 `Default()`，或唤醒后数据已经被其他 reader / writer 抢走。竞争失败是合法结果，持续等待时在外层循环重新构造 `CoSelect`。

`CaseRead` 的输出对象和引用形式的 Chan 必须活到 `Run()` 返回。`CaseWrite` 在注册时按值保存待写入值。

### 5.3 CoMutex

```cpp
#include <bbt/coroutine/sync/CoLockGuard.hpp>

auto mutex = bbtco_make_comutex();

bbtco [mutex]() {
    sync::CoLockGuard<sync::CoMutex> guard(mutex);
    UpdateSharedState();
};
```

`Lock()` 成功返回 `void`，不要把它写成 `auto locked = mutex->Lock()`。优先使用 RAII guard，避免异常和提前 return 遗漏解锁。同一个协程重复 `Lock()` 会触发死锁断言。

立即尝试使用 `TryLock()`，带超时尝试使用 `TryLock(ms)`。等待型版本必须在协程内。

### 5.4 CoCond

条件变量只负责唤醒，不保存业务谓词。等待方必须在循环中复查共享状态，并用同一把协程锁保护状态和谓词。

使用 `CoCond` 时推荐的结构是：

1. 获取 `CoMutex`。
2. 检查谓词。
3. 谓词不满足时调用 `Wait()` 或 `WaitFor(ms)`。
4. 被唤醒后重新获取锁并复查谓词。
5. 修改谓词后调用 `NotifyOne()` 或 `NotifyAll()`。

`Notify` 不携带状态，也不保证业务条件已经满足。条件变量销毁前必须保证没有等待者。

### 5.5 CoRWMutex

`CoRWMutex` 适合读多写少的共享状态。它是写者优先，有写者排队时新读者会等待。不能在持有读锁时升级为写锁，否则会形成互等。

```cpp
#include <bbt/coroutine/sync/CoLockGuard.hpp>

auto rwmutex = bbtco_make_corwmutex();

auto Read = [rwmutex]() {
    sync::CoReadLock guard(rwmutex);
    ReadSharedState();
};

auto Write = [rwmutex]() {
    sync::CoWriteLock guard(rwmutex);
    WriteSharedState();
};
```

## 6. 协程池和固定并发

### 6.1 默认协程池

`CoPool::Create(max_co)` 创建固定数量的池协程。`Submit` 只入队，不等待调用方。`Release` 停止接收新任务，等待正在执行的池协程退出，并丢弃队列中尚未执行的任务。

```cpp
auto pool = bbtco_make_copool(4);

for (int i = 0; i < 100; ++i) {
    pool->Submit([i]() {
        ProcessJob(i);
    });
}

bbtco_sleep(100);
pool->Release();
```

池必须活过全部 worker 和在途任务。不要在池自己的 worker 协程里调用 `Release()`，这会形成自等待并抛出 `std::logic_error`。

### 6.2 需要结果时使用 `SubmitWithFuture`

```cpp
auto future = pool->SubmitWithFuture([]() {
    ProduceResult();
});

if (future.valid()) {
    try {
        future.get();
    } catch (const std::exception& error) {
        Report(error);
    }
}
```

`future.get()` 会阻塞执行它的线程。它适合 native 控制面或已经明确不会占用 runtime worker 的场景。协程内需要等待任务结果时，优先使用 `Chan` 或其他协程友好的完成通知。

池任务抛出的异常不会直接穿透到 runtime。可以使用 `SetExceptionCallback` 交付异常，也可以使用 `SubmitWithFuture` 让异常在 `future.get()` 中重新抛出。没有处理的异常可通过 `GetUnhandledExceptionCount()` 观察。

### 6.3 长任务和池容量

池中每个 worker 同时只能执行一个任务。任务在 I/O 或 sleep 上挂起时，会占用这条池协程，导致可用并发度下降。任务包含长时间 CPU 计算时也会占住池协程和 worker。

需要按栈大小隔离任务时使用 `CoPoolOptions` 和 `Submit(StackRequest, ...)`。每个尺寸类的栈规格、worker 数和队列容量在池创建时固定，提交必须精确匹配已声明的 `(usable_bytes, protect)`。

## 7. async 操作

async API 有两种明确的语义，调用前先判断你的场景。

| 场景 | 定义方式 | 函数体何时执行 |
|---|---|---|
| 顺序组合多个 async 操作 | `bbt_co_async(T)` + `bbt_co_await` + `bbt_co_return` | 调用时开始 |
| 延迟执行并保存 callable / 参数 | `bbt_co_bind(...)` | 只有 `bbt_co_await` 或 `bbt_co_fire` 消费时 |

推荐把 HTTP、RPC、Redis 等多步业务流程写成顺序 async 函数。每一步用 `bbt_co_await`，业务错误在当前位置处理，最终用 `bbt_co_return` 返回。

```cpp
bbt_co_async(result<int>) LoadAndTransform(Key key)
{
    const auto raw = bbt_co_await Load(key);
    if (!raw)
        bbt_co_return(result<int>::err(raw.error()));

    const auto normalized = bbt_co_await Normalize(raw.value());
    if (!normalized)
        bbt_co_return(result<int>::err(normalized.error()));

    bbt_co_return(Transform(normalized.value()));
}
```

`bbt_co_return_void` 用于 `void`，`bbt_co_return_ref(value)` 用于借用寿命足够长的左值。顺序函数直接在调用方执行；在受管协程中用 `bbt_co_await` 调用，不要靠 `bbt_co_fire` 推迟函数体。

需要把普通函数、lambda 和参数先保存，再交给 `await` 或 `fire` 时使用：

```cpp
auto operation = bbt_co_bind(
    [](std::string value) {
        return Parse(value);
    },
    std::string(input));

const auto parsed = bbt_co_await std::move(operation);
```

`bbt_co_bind` 绑定时按值保存参数。左值会拷贝，右值可以移动。`std::string_view`、引用捕获、指针和 `std::ref` 都只是借用，不会延长对象寿命。跨等待点的字符串和只移动对象要显式复制或移动。

完整语法、返回类型约束、异常和生命周期规则见 [Async 调用指南](async-usage.md)。

## 8. 资源清理、异常和取消

### 8.1 `bbtco_defer`

`bbtco_defer` 在所在 C++ 作用域退出时执行，包括 return 和异常展开。它是 RAII 对象的语法糖，不是 runtime 关闭回调。

```cpp
bbtco []() {
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;

    bbtco_defer { close(fd); };
    ReadFile(fd);
};
```

多个 defer 按后声明先执行。已有类型析构已经能表达清理时，优先使用普通 RAII。defer 默认引用捕获，资源和引用对象必须活到作用域退出。

### 8.2 异常

受管协程中的异常不会传播到创建它的 native 调用栈。detached 协程的异常由 runtime 或 `CoPool` 按各自契约记录、计数或交给异常回调。需要把业务错误交还给调用方时，优先使用 `result<T>`、返回码、`Chan` 或 `SubmitWithFuture`，不要依赖 detached 异常穿透线程边界。

应用需要统一观察未处理异常时，配置 `m_ext_coevent_exception_callback` 或为 `CoPool` 设置异常回调。异常回调本身也必须短小且不得阻塞整个 worker。

### 8.3 协作式取消

取消不会强杀协程。业务代码应在循环、I/O 返回和其他自然检查点检查取消状态，然后通过普通 return 释放 RAII 资源。不要假设一个正在运行的 native 阻塞调用会被强制打断。

## 9. 栈、性能和线程边界

### 9.1 栈大小

默认协程栈当前为 12 KiB。栈大小创建后固定，不支持扩栈或换栈。深递归、大的局部数组、重型日志和第三方库调用需要显式提高栈预算。

推荐顺序如下：

1. 先用 `GetLocalCoroutineStackSize()` 观察实际执行协程的可用栈。
2. 用 `RuntimeOptions::default_stack` 或显式注册入口调整默认栈。
3. 任务栈需求差异明显时使用尺寸路由 `CoPool`。
4. 使用 `examples/stack_budget/` 做专门的栈预算分析。

不要把大对象放在小协程栈上。把大缓存、可变长数据和长期状态放到堆上，并使用明确的 owner。

### 9.2 worker 不是业务线程

协程可能迁移 worker，所以以下做法不安全：

- 把 `std::thread::id` 当成协程身份。
- 把 worker 线程的 TLS 指针保存到跨等待状态。
- 持有 `std::mutex` 或其他线程锁后挂起协程。
- 假设一次 `bbtco_yield` 后仍回到同一个 worker。
- 让一个 native 线程长期阻塞等待 runtime worker。

需要跨挂起点保护业务状态时使用 `CoMutex`、`CoRWMutex`、`Chan` 或明确的消息传递。

### 9.3 CPU 任务

调度是协作式的。纯 CPU 任务要主动拆分并使用 `bbtco_yield`，或者投递到合适的执行池。不要在协程里运行无界循环，也不要把长时间 CPU 计算伪装成异步 I/O。

worker 卡顿告警可通过 `m_cfg_worker_stall_warn_ms` 观察。性能优化前先按 [性能指南](performance.md) 的口径测量，不凭单次耗时判断调度器行为。

## 10. 常见场景配方

### 10.1 顺序调用多个 I/O 操作

下一步依赖上一步的结果时，用顺序 async 函数；不要把其中一步改成 `fire`。见[顺序组合示例](#7-async-操作)。

### 10.2 延迟提交副作用

需要先构造操作，稍后由当前协程等待或 detached 提交时使用 `bbt_co_bind`。只有明确放弃最终结果时才用 `bbt_co_fire`。

```cpp
auto audit = bbt_co_bind(
    [](std::string message) {
        WriteAuditLog(message);
    },
    std::string(message));

bbt_co_fire std::move(audit);
```

提交动作本身可能因 scheduler 未初始化或提交失败在调用点抛异常。业务仍需处理提交边界。

### 10.3 生产者消费者

使用 `Chan` 传递任务，以 `Close()` 表示不会再有新任务。消费者读到关闭且无残留数据时退出。生产者和消费者之间不共享临时栈引用。

### 10.4 共享状态通知

使用 `CoMutex` 保护状态，`CoCond` 负责唤醒。等待方循环检查谓词，通知方先修改状态再通知。不要把 Notify 次数当成业务状态。

### 10.5 多来源事件

多个缓冲 `Chan` 同时等待时使用 `CoSelect`。`Run()` 返回 `-1` 时重新检查关闭、超时和竞争状态，不能把 `-1` 直接解释成单一原因。

### 10.6 有界并发

使用 `CoPool` 固定 worker 数。提交失败必须检查返回码，释放池前停止提交新任务。需要等待所有任务完成时，让任务通过 `Chan` 或 future 报告结果，再由池外的控制协程调用 `Release()`。

### 10.7 事件驱动的 fd 服务

监听协程使用 Hook 版 `accept` / `read` / `write`，或使用 `bbtco_wait_for` 明确等待事件。每个连接的 fd 生命周期用 RAII 或 `bbtco_defer` 管理。需要一次性事件回调时使用 `bbtco_ev_*`。

## 11. 常见错误对照表

| 错误写法 | 结果 | 改法 |
|---|---|---|
| `bbtco` 中 `[&]` 捕获局部变量后立即返回 | 协程执行时引用可能悬空 | 值捕获、移动捕获或 `shared_ptr` |
| 在 native 线程调用 `Chan::Read`、`Lock`、`Wait` | 不满足协程等待前提 | 把调用放入 `bbtco` 或使用 native 同步 API |
| 用 `std::mutex` 持锁后调用可能挂起的 API | 阻塞其他线程或形成锁顺序问题 | 使用 `CoMutex`，或在挂起前释放 native 锁 |
| 把 `future.wait/get` 放进 runtime worker | worker 被 native 阻塞 | 用 `Chan` / `CoCond` / 协程 async 组合 |
| 假设 detached 协程按注册顺序执行 | 执行顺序未定义 | 用 Chan、条件变量或显式状态同步 |
| 把进程返回当作任务收口 | detached 任务可能尚未完成 | 主任务内等待完成，再返回 |
| 在 `CoPool` worker 内调用 `Release()` | 自等待，抛 `std::logic_error` | 由池外控制协程或 native 控制面释放 |
| `CoSelect` 使用 `Chan<T, 0>` | 编译期拒绝 | 使用缓冲 Chan |
| 顺序 async 函数依赖 `bbt_co_fire` 延迟执行 | 函数体已经在调用时开始执行 | 需要延迟时改用 `bbt_co_bind` |
| `bbt_co_bind` 捕获 `string_view` 指向临时字符串 | 消费时可能访问悬空内存 | 按值保存 `std::string` |
| 在协程中无限 CPU 循环 | 占住 worker，不会自动让出 | 拆分工作并 `bbtco_yield`，或使用合适的池 |
| 为了使用 Hook 手动修改 fd flags | 破坏调用方和库的 flags 约定 | 保持 fd 配置，让 Hook 管理协程 I/O |

## 12. 文档和示例地图

| 需求 | 入口 |
|---|---|
| 第一次使用和场景选择 | 本文 |
| async、bind、await、fire | [Async 调用指南](async-usage.md) |
| 完整签名、返回码和挂起条件 | [API 参考](api-reference.md) |
| 运行时、生命周期和网络执行域 | [核心运行时契约](../architecture/core-runtime-contract.md) |
| 测试分层和本地验证 | [测试指南](testing.md) |
| 性能测量和回归 | [性能指南](performance.md) |
| 栈预算分析 | [栈预算指南](stack-budget.md) |
| CI、发布和真实客户端验收 | [CI 指南](ci.md)、[真实客户端验收](real-client-acceptance.md) |
| 宏速查和每个宏的底层语义 | `bbt/coroutine/syntax/SyntaxMacro.hpp` |
| 运行时入口宏 | `bbt/coroutine/syntax/RuntimeMacros.hpp` |
| 可编译示例 | `examples/` |
| 调用方 Agent 规则 | `.github/skills/bbtools-coroutine/SKILL.md` |

推荐先运行 `examples/` 中与目标场景相近的示例，再读取 API 参考核对返回码和生命周期。示例只展示调用方式，公共行为的断言和回归覆盖在 `tests/` 中。
