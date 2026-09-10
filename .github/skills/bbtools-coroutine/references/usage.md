# 用法

签名与返回码见 `agent-docs/api-reference.md`。对照可编译示例：`example/`。

入口：`#include <bbt/coroutine/coroutine.hpp>`。Linux，C++17。

---

## 基本用法

### 启动、注册、停止

```cpp
#include <bbt/coroutine/coroutine.hpp>
#include <unistd.h>
using namespace bbt::coroutine;

int main()
{
    g_scheduler->Start();

    bbtco [](){
        printf("co %lu\n", GetLocalCoroutineId());
        bbtco_sleep(50);
        bbtco_yield;
    };

    sleep(1);
    g_scheduler->Stop();
    return 0;
}
```

对照：`example/coroutine.cc`。

- 默认 `Start()` = 后台调度线程。返回后即可 `bbtco`。
- `bbtco` 是 detached，注册后不保证执行顺序。
- 主线程要用普通 `sleep`/`latch` 等协程跑一段时间；`Stop()` 不会等业务结束。
- 给关键协程起名：`bbtco_desc("worker") [](){};`

### 让出与睡眠

- `bbtco_sleep(ms)`：协程内挂起指定毫秒，不占死 worker。
- `bbtco_yield`：立刻让出，重新入就绪队列。
- `GetLocalCoroutineId()`：非协程上下文返回 `0`。

### 注册失败

停机后：`bbtco` 抛 `std::runtime_error`。需要不抛：

```cpp
bool succ = false;
bbtco_noexcept(&succ) [](){};
```

---

## 进阶用法

### Chan

```cpp
auto chan = sync::Chan<int, 8>{};
chan << 1;          // Write，满则挂起
int v = 0;
chan >> v;          // Read，空则挂起
chan.Close();
```

- `N>0` 有缓冲；`N=0` 无缓冲（写等到读端取走）。
- `Write`/`Read` 返回 `0` / `-1`（关闭）/ `-2`（错误）。`<<`/`>>` 只表示是否成功。
- 带超时：`TryRead(item, ms)` / `TryWrite(item, ms)`，超时返回 `1`。

对照：`example/chan.cc`。

### 锁与条件变量

```cpp
auto mu = bbtco_make_comutex();
sync::CoLockGuard<sync::CoMutex> g(mu);   // 构造加锁，析构解锁
```

读写锁：`bbtco_make_corwmutex()`，RAII 用 `CoReadLock` / `CoWriteLock`。写优先：有等待写者时新读者会挂起。

```cpp
auto cv = bbtco_make_cocond();
cv->Wait();         // 必须在协程内
cv->NotifyOne();
```

### CoSelect

```cpp
int idx = sync::CoSelect()
    .CaseRead(ch1, out)
    .CaseWrite(ch2, val)
    .CaseTimeout(100)
    .Run();
```

`idx` 是命中的 Case 下标；`-1` 为超时、`Default` 或竞争失败。只支持有缓冲 Chan。必须在协程内 `Run()`。

### 事件等待

```cpp
bbtco_wait_for(fd, bbtco_emev_readable, 1000);
```

必须在协程内。失败抛 `std::runtime_error`。纯超时可用 `bbtco_emev_timeout`。对照：`example/waitfor.cc`。

`bbtco_ev_r(fd)` 等：事件就绪后再注册一个协程处理。见 `SyntaxMacro.hpp`。

### Hook IO

协程里直接 `read`/`write`/`connect`/`poll`/`sleep`/`getaddrinfo` 等，由库转成等待。不要为此改 FD flags。非协程线程走原生实现。

### Defer

```cpp
bbtco_defer { cleanup(); };
```

后声明的先执行，与 C++ 析构顺序一致。

### 配置

`g_bbt_coroutine_config` 在 **`Start` 之前**改。常用：`m_cfg_stack_size`（默认 12 KiB）、`m_cfg_static_thread_num`（`0` 表示 `hardware_concurrency()`）、`m_cfg_stack_protect`、`m_ext_coevent_exception_callback`、`m_cfg_worker_stall_warn_ms`。

---

## 使用范式

### 生产 / 消费

一个（或一组）协程 `Write`，另一组 `Read`，结束时 `Close()`，读端以失败返回退出。对照：`example/consumer_productor.cc`、`example/chan.cc`。

### 锁 + 条件变量

改共享状态时持 `CoLockGuard`；等待方循环检查谓词并 `Wait()`；修改方改完后 `NotifyOne`/`NotifyAll`。不要用 `Lock()` 的返回值。

### CoPool

固定数量的池协程消费任务队列：

```cpp
auto pool = bbtco_make_copool(4);
pool->Submit([](){ /* 协程内 */ });
auto fut = pool->SubmitWithFuture([](){});
pool->Release();   // 停止接任务；等运行中的退出；丢掉未执行的
if (fut.valid()) {
    try { fut.get(); }
    catch (const std::future_error&) {}
}
```

任务里长时间 `sleep`/IO 会占住那条池协程。对照：`example/copool.cc`。

### 事件驱动 IO

协程内 `accept`/`read` 即可；或 `bbtco_wait_for` 明确等 fd。需要「事件到了再起协程」时用 `bbtco_ev_*`。对照：`example/echoserver/`、`example/waitfor.cc`。
