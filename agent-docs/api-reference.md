# bbtools-coroutine API 参考

当前用户可调用行为。签名以头文件为准；本文与头文件冲突时改本文。

时间单位未另写时为**毫秒**。返回码未另写时：**`0` 成功、`-1` 失败、`1` 超时**。

未列入覆盖范围的 `detail/` 成员不当作用户 API。

## 覆盖范围

- 运行时：`g_scheduler`、`Start` / `Stop` / `LoopOnce` / `IsRunning`、`SchedulerStartOpt`
- 注册：`bbtco` / `bbtco_desc` / `bbtco_ref` / `bbtco_noexcept`
- 让出：`bbtco_yield`、`bbtco_sleep`
- 查询：`GetLocalCoroutineId`、`GetLocalCoroutineStackSize`
- 同步：`Chan`、`CoSelect`、`CoMutex`、`CoRWMutex`、`CoLockGuard`、`CoUniqueLock`、`CoReadLock`、`CoWriteLock`、`CoCond`
- 池：`CoPool`
- 事件：`bbtco_wait_for`、`bbtco_ev_*`
- Defer：`bbtco_defer`
- Hook 拦截列表与 IO 约束
- 配置：`g_bbt_coroutine_config` 用户字段

## 本文不覆盖

`CoWaiter`、`StdLockWapper`、`DnsResolver`、`Profiler`、`DebugMgr`、`CoPoller`、`Processer`、`IChan` / `ICoLock`、`bbtco_ev_*_with_copool` 逐项说明、Hook 每个 syscall 的 errno 矩阵（见 `unit_test/hook_contract.hpp`）。

---

## 1. 运行时入口

### `g_scheduler`

- 头文件：`bbt/coroutine/detail/Define.hpp`、`bbt/coroutine/detail/Scheduler.hpp`
- 签名：`#define g_scheduler (bbt::coroutine::detail::Scheduler::GetInstance())`
- 说明：全局单例。`GetInstance()` 返回 `std::unique_ptr<Scheduler>&`。

### `Scheduler::Start`

- 头文件：`Scheduler.hpp`
- 签名：`void Start(SchedulerStartOpt opt = SCHE_START_OPT_SCHE_THREAD);`
- 前置：进程内只启动一次；`THREAD` 模式 assert `m_sche_thread == nullptr`。
- 选项（`Define.hpp`）：

| 值 | 行为 |
|----|------|
| `SCHE_START_OPT_SCHE_THREAD`（默认） | 后台调度线程，`Start` 在 worker 创建后返回 |
| `SCHE_START_OPT_SCHE_LOOP` | 当前线程阻塞跑调度循环 |
| `SCHE_START_OPT_SCHE_NO_LOOP` | 只创建 worker，由调用方 `LoopOnce` 驱动 |

### `Scheduler::LoopOnce`

- 签名：`void LoopOnce();`
- 前置：仅 `NO_LOOP`。已 `Start(THREAD)` 时 assert。

### `Scheduler::IsRunning`

- 签名：`bool IsRunning() const noexcept;`
- 说明：`Stop` 将标志置 `false`。

### `Scheduler::Stop`

- 签名：`void Stop();`
- 语义：取消式停机。停止接新任务；join worker；回收全局队列中未执行的协程；回收 parked（fd/定时器等待）协程。不保证业务任务执行完。
- 挂起协程在 Stop 时被**直接销毁，不做栈展开**（契约 §6 显式例外）：栈上对象不执行析构与 RAII。需要可靠清理的资源必须在挂起点之前释放或改由栈外管理。
- 停机后 `RegistCoroutineTask`（非 noexcept）抛 `std::runtime_error("scheduler stopped: coroutine task rejected")`。

### `Scheduler::RegistCoroutineTask`

- 签名：
  - `void RegistCoroutineTask(const CoroutineCallback& handle, const char* desc = nullptr);`
  - `void RegistCoroutineTask(const CoroutineCallback& handle, bool& succ) noexcept;`
- 前置：调度器运行中。超过 `m_cfg_max_coroutine` 会抛异常。
- `desc`：写入协程，诊断可读回。空表示未命名。

---

## 2. 协程注册宏

头文件：`bbt/coroutine/syntax/SyntaxMacro.hpp`。宏映射到 `RegistCoroutineTask`，不另起状态机。

### `bbtco`

- 映射：`_CoHelper()-` → `RegistCoroutineTask(func, desc=nullptr)`
- 用法：`bbtco [](){ ... };` 或 `bbtco [&](){ ... };`
- 执行：detached，任意时刻任意 worker。不保证顺序。
- 必须在 `Start` 之后。停机后抛 `runtime_error`。

### `bbtco_desc(desc)`

- 映射：`_CoHelper(desc)-`，`desc` 为 `const char*`
- 用法：`bbtco_desc("worker") [](){};`
- 与 `bbtco` 相同，描述写入协程。

### `bbtco_ref`

- 定义：`#define bbtco_ref bbtco [&]()`
- 用法：`bbtco_ref { ... };`（块，不是 `[]()`）。
- 捕获：引用捕获，生命周期由调用方保证。

### `bbtco_noexcept(succ)`

- 映射：`_CoHelper(succ)+` → `RegistCoroutineTask(func, bool&)`
- 用法：`bool succ; bbtco_noexcept(&succ) [](){};`
- 失败：`succ=false`，不抛。成功：`succ=true`。
- 参数是 `bool*`。

---

## 3. 让出、睡眠、查询

### `bbtco_yield`

- 映射：当前 TLS 协程 `YieldAndPushGCoQueue()`。无当前协程则空操作。
- 让出后进入就绪队列，稍后可能在其他 worker 恢复。

### `bbtco_sleep(ms)`

- 映射：`detail::Hook_Sleep(ms)`
- 协程内：挂起指定毫秒后唤醒。
- 非协程：原生睡眠。应在协程内使用。

### `GetLocalCoroutineId`

- 头文件：`bbt/coroutine/coroutine.hpp`
- 签名：`detail::CoroutineId GetLocalCoroutineId();`
- 返回：当前协程 id；非协程上下文为 `0`。

### `GetLocalCoroutineStackSize`

- 签名：`size_t GetLocalCoroutineStackSize();`
- 返回：当前协程栈大小；非协程为 `0`。

### 取消

- 头文件：`bbt/coroutine/detail/Coroutine.hpp`
- 签名：`void RequestCancel() noexcept;` / `bool IsCancelRequested() const noexcept;`
- 当前协程：`g_bbt_tls_coroutine_co`（`Define.hpp`）。非协程为 `nullptr`。
- 不强杀。运行中代码在检查点或等待返回后自行退出，RAII 照常。

### 诊断

- `const std::string& GetDescription() const noexcept` — `bbtco_desc` 写入的名字，未命名为空串。
- `int GetWaitInfo(CoroutineWaitInfo& out) const noexcept` — 仅协程自身、同 Processer 线程可读。

---

## 4. Chan

头文件：`bbt/coroutine/sync/Chan.hpp`，实现 `__TChan.hpp`。

### `sync::Chan<TItem, Max>`

- `Max > 0`：有缓冲。`Max == 0`：无缓冲，写等到读端取走。
- 构造：`sync::Chan<int, 8>{}` 或 `bbt::coroutine::Chan<int, 8>()`（返回 `SPtr`）。
- 不可拷贝。

| 方法 | 挂起 | 返回 |
|------|------|------|
| `int Write(const T&)` | 缓冲满（或无缓冲未配对） | `0` 成功，`-1` 已关闭，`-2` 错误 |
| `int Read(T&)` | 空 | 同上 |
| `int ReadAll(vector<T>&)` | 会挂起 | 同上 |
| `int TryWrite(const T&)` | 否 | 同上 |
| `int TryRead(T&)` | 否 | 同上 |
| `int TryWrite(const T&, int timeout)` | 最多 `timeout` ms | 另加 `1` 超时 |
| `int TryRead(T&, int timeout)` | 最多 `timeout` ms | 另加 `1` 超时 |
| `void Close()` | 否 | 关闭后读失败 |
| `bool IsClosed()` | 否 | |
| `size_t size() const` | 否 | 当前缓冲数量 |
| `size_t capacity() const` | 否 | `Max` |

- `bool operator<<(Chan&, const T&)` / `operator>>` 及 `shared_ptr` 重载：等价 `Write`/`Read == 0`。
- 析构：未关闭则 `Close()`。
- `Add*Watcher`：供 `CoSelect` 使用。
- 会挂起的读写必须在协程内。

---

## 5. CoSelect

头文件：`bbt/coroutine/sync/CoSelect.hpp`

```cpp
int idx = sync::CoSelect()
    .CaseRead(ch1, out)
    .CaseWrite(ch2, val)
    .CaseTimeout(100)
    .Run();
```

- `Run()` 必须在协程内。返回命中 Case 的注册下标；`-1` = timeout / `Default` / 唤醒后全未就绪。
- `CaseTimeout(ms)` 与 `Default()` 同时出现时 Default 优先（不挂起）。
- `Default()`：全部未就绪立即 `-1`。
- 仅 `Max > 0`。`Chan<T,0>` 为 `static_assert`。
- 引用重载：Chan 与 `out` 活到 `Run()` 返回。`SPtr` 重载在该表达式内保活；跨语句需自行持有。

---

## 6. CoMutex / 读写锁 / RAII

### `sync::CoMutex`

- 头文件：`bbt/coroutine/sync/CoMutex.hpp`
- 创建：`bbtco_make_comutex()` → `SPtr`
- 会等待的加锁必须在协程内。

| 方法 | 返回 |
|------|------|
| `void Lock()` | 直到获得；同协程重入 assert |
| `void UnLock()` | |
| `int TryLock()` | `0` 成功，`-1` 未获得 |
| `int TryLock(int ms)` | `0` 成功，`1` 超时，`-1` 错误 |

### `sync::CoLockGuard<Mutex>` / `CoUniqueLock<Mutex>`

- 头文件：`bbt/coroutine/sync/CoLockGuard.hpp`
- `CoLockGuard`：构造 `Lock()`，析构 `UnLock()` noexcept。持有 `shared_ptr`。不可拷贝、不可移动。
- `CoUniqueLock`：`defer_lock` / `try_to_lock` / `adopt_lock` / `try_lock_for(ms)` / 可移动。
- `try_lock()` / `try_lock_for` 返回 `bool`（`TryLock==0`）。

### `sync::CoRWMutex`

- 头文件：`bbt/coroutine/sync/CoRWMutex.hpp`
- 创建：`bbtco_make_corwmutex()`
- 写优先：有等待写锁时新读者挂起。
- `RLock` / `WLock` 成功返回 `0`。持读锁再取写锁 assert。
- `TryRLock` / `TryWLock`：`0` 成功，`-1` 会阻塞。带 `ms`：`0` / `1` / `-1`。
- RAII：`CoReadLock` / `CoWriteLock`（同一头文件）。

---

## 7. CoCond

- 头文件：`bbt/coroutine/sync/CoCond.hpp`
- 创建：`bbtco_make_cocond()` → `SPtr`
- `Wait` 必须在协程内。

| 方法 | 返回 |
|------|------|
| `int Wait()` | `0` 被唤醒，`-1` 失败 |
| `int WaitFor(int ms)` | `0` 唤醒，`1` 超时，`-1` 失败 |
| `int NotifyOne()` | `0` 唤醒了一个，`-1` 当时无等待者 |
| `void NotifyAll()` | 唤醒当前队列全部 |

析构时若队列非空，打调试日志。

---

## 8. CoPool

- 头文件：`bbt/coroutine/pool/CoPool.hpp`
- 创建：`bbtco_make_copool(pool_max_co)`，协程数固定。

| 方法 | 行为 |
|------|------|
| `int Submit(const CoPoolWorkCallback&)` / `&&` | `0` 入队，`-1` 池已停或分配/入队失败 |
| `std::future<void> SubmitWithFuture(...)` | 空 future = 提交失败；任务异常在 `get()` 抛出 |
| `void SetExceptionCallback(function<void(exception_ptr)>)` | `nullptr` 恢复仅计数 |
| `uint64_t GetUnhandledExceptionCount() const noexcept` | 未被回调/future 接住的异常数 |
| `void Release()` | 停止 `Submit`；等运行中协程退出；丢弃未执行任务；带 future 的以 `broken_promise` 兑现。可重复调用 |

池内任务挂起占用那条池协程。任务异常被隔离，不打爆调度器。

---

## 9. 事件等待

### `bbtco_wait_for(fd, event, ms)`

- 头文件：`SyntaxMacro.hpp`；实现 `_WaitForHelper.cc`
- 前置：必须在协程内，否则 assert。
- 行为：构造即挂起，直到 fd 事件或超时。失败抛 `std::runtime_error("wait for failed!")`。
- `event`：`bbtco_emev_readable` / `writeable` / `timeout` / `close` / `finalize` / `persist`（`bbt::pollevent::EventOpt`）。

### `bbtco_ev_*`

事件就绪后再注册一个协程：

| 宏 | 含义 |
|----|------|
| `bbtco_ev_r(fd)` / `bbtco_ev_w(fd)` | 可读 / 可写，无超时 |
| `bbtco_ev_rc` / `bbtco_ev_wc` | 带 finalize |
| `bbtco_ev_rt(fd, timeout_ms)` / `bbtco_ev_wt` | 带超时 |
| `bbtco_ev_t(timeout_ms)` | 纯超时，`fd=-1` |

`with_copool` 变体把回调提交到指定 `CoPool`。定义见 `SyntaxMacro.hpp`。

---

## 10. Defer

### `bbtco_defer`

- 映射：`detail::Defer`，后声明的先执行。
- 用法：`bbtco_defer { cleanup(); };` 在协程内。

---

## 11. Hook / I/O

头文件：`bbt/coroutine/detail/Hook.hpp`（随 `coroutine.hpp` 引入）。

协程上下文拦截并转为协程等待；非协程直通原生。拦截：

- 套接字：`socket` / `connect` / `accept` / `accept4` / `close` / `read` / `write` / `send` / `recv` / `sendto` / `recvfrom` / `sendmsg` / `recvmsg` / `readv` / `writev`
- 时间：`sleep` / `usleep` / `nanosleep` / `clock_nanosleep`
- 多路复用：`poll` / `select` / `pselect`
- DNS：`getaddrinfo` / `getnameinfo` / `gethostbyname` / `gethostbyaddr`

约束：

- 协程 IO 期间临时 `O_NONBLOCK`，返回时恢复原 flags。
- `MSG_DONTWAIT` 立即返回。
- `SO_RCVTIMEO` / `SO_SNDTIMEO` 按协程 deadline 有界返回 `-1`/`EAGAIN`。
- 等待中 fd 被 close：唤醒后重试 syscall，得到 `EBADF`。
- 不支持多线程对同一 fd 并发走 Hook IO。

---

## 12. 配置

- 头文件：`bbt/coroutine/detail/GlobalConfig.hpp`
- 访问：`g_bbt_coroutine_config`
- 非线程安全，`Start` 前设置。

| 字段 | 默认 | 说明 |
|------|------|------|
| `m_cfg_max_coroutine` | `65535` | 超过再注册抛异常 |
| `m_cfg_static_coroutine` | `0` | 启动时预创建数量 |
| `m_cfg_stack_size` | `1024 * 12` | 12 KiB |
| `m_cfg_stack_protect` | `true` | 保护页，溢出 `SIGSEGV` |
| `m_cfg_static_thread_num` | `0` → `hardware_concurrency()` | worker 数 |
| `m_cfg_scan_interval_ms` | `1` | 调度扫描间隔 |
| `m_ext_coevent_exception_callback` | `nullptr` | detached 异常回调；回调不得再抛给运行时 |
| `m_unhandled_exception_count` | `0` | 无回调时计数 |
| `m_cfg_worker_stall_warn_ms` | `0` 关 | worker 单次执行超过该毫秒告警 |
| `m_ext_worker_stall_callback` | 默认 stderr | 调度线程调用，勿阻塞 |

`m_cfg_processer_*` 与栈池参数是内部调优，见头文件。
