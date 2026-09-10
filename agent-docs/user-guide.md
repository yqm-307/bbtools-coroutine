# bbtools-coroutine 使用手册

**状态：** 对应当前 `main` 用户可依赖行为（#323）
**签名真源：** 头文件；本文只讲怎么组合
**契约真源：** `agent-docs/2026-09-07-core-runtime-contract.md`（目标语义，不等于已全部实现）
**按符号查阅：** `agent-docs/api-reference.md`

Linux only。C++17。依赖 boost.context 与 bbtools-core。

## 1. 最小程序

```cpp
#include <bbt/coroutine/coroutine.hpp>
#include <unistd.h>
using namespace bbt::coroutine;

int main()
{
    g_scheduler->Start();          // 默认后台调度线程

    bbtco [](){
        printf("co %lu\n", GetLocalCoroutineId());
        bbtco_sleep(50);
    };

    sleep(1);                      // 普通线程等协程跑一会儿
    g_scheduler->Stop();           // 取消式停机，不保证业务跑完
    return 0;
}
```

对照：`example/coroutine.cc`。构建见 README「一、安装」。

`Start()` 返回后即可 `bbtco`。默认 `SCHE_START_OPT_SCHE_THREAD`：不要再调 `LoopOnce()`。

## 2. 常见组合

### 2.1 Chan 生产 / 消费

```cpp
auto chan = sync::Chan<int, 8>{};

bbtco [&](){
    for (int i = 0; i < 8; ++i)
        chan << i;                 // 缓冲满则挂起
    chan.Close();
};

bbtco [&](){
    int v = 0;
    while ((chan >> v))            // Read 成功为 true
        printf("%d\n", v);
};
```

对照：`example/chan.cc`。

- `Chan<T, N>`，`N>0` 有缓冲；`N=0` 无缓冲（写阻塞到读端取走）。
- `Write`/`Read`：`0` 成功，`-1` 已关闭，`-2` 错误。`Try*` 超时版另返回 `1`。
- `<<` / `>>` 只表示是否 `==0`，丢失 `-1`/`-2` 细节。
- `CoSelect` **不支持** `Chan<T,0>`。

### 2.2 锁 + 条件变量

```cpp
auto mu = bbtco_make_comutex();
auto cv = bbtco_make_cocond();
bool ready = false;

bbtco [&](){
    bbtco_sleep(50);
    {
        sync::CoLockGuard<sync::CoMutex> g(mu);
        ready = true;
    }
    cv->NotifyOne();
};

bbtco [&](){
    sync::CoLockGuard<sync::CoMutex> g(mu);
    while (!ready)
        cv->Wait();                // 必须在协程内
};
```

- `Lock()` 是 `void`，不要写成 `auto x = mu->Lock()`。
- 用 `CoLockGuard` / `CoUniqueLock`，不要手写 `Lock`/`UnLock` 漏释放。
- `CoCond` 析构时若仍有等待者，会打调试日志；先唤醒或保证等待者已退出。

### 2.3 CoPool 提交与停机

```cpp
auto pool = bbtco_make_copool(4);
auto fut = pool->SubmitWithFuture([](){ bbtco_sleep(10); });

pool->Release();                   // 停止接任务；运行中的等退出；队列里未执行的丢掉
try {
    if (fut.valid()) fut.get();    // 被排空的任务可能 broken_promise
} catch (const std::future_error&) {}
```

对照：`example/copool.cc`（其中「阻塞直到全部退出」的旧注释以 `Release` 契约为准）。

- 池协程数固定。任务里长时间挂起会占住 worker。
- `Submit`：`0` 成功，`-1` 池已停或分配失败。
- 任务异常：`SubmitWithFuture` 经 `future.get()` 抛出；否则计数或交给 `SetExceptionCallback`。

## 3. 禁区

1. **栈引用：** `bbtco` 是 detached。捕获局部变量引用时，该协程必须在变量销毁前结束（或改堆/`shared_ptr`）。
2. **`Stop` ≠ 排空：** 停机不保证业务完成；parked（fd/定时器）协程被唤醒销毁；未执行任务回收。要完成语义，停机前自己等 latch。
3. **`Stop` 后注册：** `bbtco` 抛 `runtime_error`；`bbtco_noexcept` 的 `succ=false`。
4. **`Start(THREAD)` + `LoopOnce`：** 会 assert。手动驱动用 `SCHE_START_OPT_SCHE_NO_LOOP`。
5. **`CoSelect` + 无缓冲 Chan：** 编译期拦截。
6. **Hook flags：** 协程 IO 期间库会临时 `O_NONBLOCK`，返回时恢复。多线程共享同一 fd 做 IO 的 flags 竞态是契约排除项。
7. **非协程里调挂起 API：** `bbtco_wait_for` 会 assert「must be in coroutine context」。`bbtco_sleep` 走 Hook，非协程则原生 `sleep` 语义（毫秒参数，见 API 参考）。
8. **在池里塞大量 CPU 死循环：** 占住 worker，别的协程饿死。需要时设 `m_cfg_worker_stall_warn_ms`。

## 4. 返回码与异常

模块内部约定：**`0` 成功、`-1` 错误、`1` 超时**。`Chan` 额外 `-2` 表示等待/内部错误。

detached 协程抛异常：保存 `exception_ptr`；无回调时日志 + `m_unhandled_exception_count`，不 terminate。生产建议设 `g_bbt_coroutine_config->m_ext_coevent_exception_callback`。

## 5. 配置（启动前）

`g_bbt_coroutine_config` 非线程安全，**`Start` 前设置**：

| 字段 | 默认 | 说明 |
|------|------|------|
| `m_cfg_stack_size` | `1024 * 12`（12 KiB） | 协程栈。README 旧文「默认 2MB」与头文件不一致，以头文件为准 |
| `m_cfg_stack_protect` | `true` | 栈底保护页，溢出 SIGSEGV |
| `m_cfg_max_coroutine` | `65535` | 超过再注册会抛异常 |
| `m_cfg_static_thread_num` | `0` → `hardware_concurrency()` | worker 数 |
| `m_cfg_worker_stall_warn_ms` | `0` 关闭 | 生产可设 500–1000 |

## 6. 文档分工

| 文件 | 职责 |
|------|------|
| `README.md` | 安装、入门示例、速查表、迁移表 |
| `agent-docs/user-guide.md` | 本文：怎么用、怎么组合、不能做什么 |
| `agent-docs/api-reference.md` | 按符号签名与返回码 |
| `agent-docs/2026-09-07-core-runtime-contract.md` | 目标契约，改运行时前读 |
| `.github/skills/bbtools-coroutine/SKILL.md` | Agent 写调用代码时的短规则 |

写调用本库的代码时加载 skill，并打开本手册与 API 参考。改 Scheduler/Hook/Stop 实现时读契约，不要用本文覆盖契约。
