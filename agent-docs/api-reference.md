# bbtools-coroutine API 参考

**状态：** 用户 API 第一版（#323），对应当前实现
**签名真源：** 下表「头文件」列
**目标契约：** `agent-docs/2026-09-07-core-runtime-contract.md`（冲突时记录差异，不改契约）
**怎么用：** `agent-docs/user-guide.md`

时间单位未另写时为**毫秒**。返回码未另写时：**`0` 成功、`-1` 失败、`1` 超时**。

未列入「覆盖范围」的 `detail/` 成员不当作用户 API。未核实项标 **待核实**。

## 覆盖范围

- 运行时：`g_scheduler`、`Start` / `Stop` / `LoopOnce` / `IsRunning`、`SchedulerStartOpt`
- 注册：`bbtco` / `bbtco_desc` / `bbtco_ref` / `bbtco_noexcept`
- 让出：`bbtco_yield`、`bbtco_sleep`
- 查询：`GetLocalCoroutineId`、`GetLocalCoroutineStackSize`
- 同步：`Chan`、`CoSelect`、`CoMutex`、`CoRWMutex`、`CoLockGuard`、`CoUniqueLock`、`CoReadLock`、`CoWriteLock`、`CoCond`
- 池：`CoPool`
- 事件：`bbtco_wait_for`、`bbtco_ev_*`
- Defer：`bbtco_defer`
- Hook 边界（拦截列表，不是逐函数百科）
- 配置：`g_bbt_coroutine_config` 用户字段

## 未覆盖（第一版）

`CoWaiter`、`StdLockWapper`、`DnsResolver`、`Profiler`、`DebugMgr`、`CoPoller`、`Processer`、`IChan`/`ICoLock`、`bbtco_ev_*_with_copool` 细节、Hook 每个 syscall 的 errno 矩阵（见单测 `hook_contract.hpp` / Issue #256）。

---

## 1. 运行时入口

### `g_scheduler`

- 头文件：`bbt/coroutine/detail/Define.hpp`（宏）、`bbt/coroutine/detail/Scheduler.hpp`
- 签名：`#define g_scheduler (bbt::coroutine::detail::Scheduler::GetInstance())`
- 说明：全局单例。本阶段不做多实例。`GetInstance()` 返回 `std::unique_ptr<Scheduler>&`。

### `Scheduler::Start`

- 头文件：`Scheduler.hpp`
- 签名：`void Start(SchedulerStartOpt opt = SCHE_START_OPT_SCHE_THREAD);`
- 前置：进程内只应启动一次；`THREAD` 模式 assert `m_sche_thread == nullptr`。
- 选项（`Define.hpp`）：

| 值 | 行为 |
|----|------|
| `SCHE_START_OPT_SCHE_THREAD`（默认） | 后台调度线程，`Start` 在 worker 创建后返回 |
| `SCHE_START_OPT_SCHE_LOOP` | 当前线程阻塞跑调度循环 |
| `SCHE_START_OPT_SCHE_NO_LOOP` | 只创建 worker，由调用方 `LoopOnce` 驱动 |

- 依据：`Scheduler.cc` `Start`；测试 `unit_test/Test_scheduler_api.cc`

### `Scheduler::LoopOnce`

- 签名：`void LoopOnce();`
- 前置：仅 `NO_LOOP`。`THREAD` 已启动时 assert。
- 依据：`Scheduler.cc`；头文件注释「Start(THREAD) 后不要 LoopOnce」

### `Scheduler::IsRunning`

- 签名：`bool IsRunning() const noexcept;`
- 说明：`Stop` 将标志置 false。注册前可查。

### `Scheduler::Stop`

- 签名：`void Stop();`
- 语义（当前实现，#280）：取消式停机。停止接新任务；join worker；回收全局队列未执行协程；回收 parked 协程。**不保证业务完成，不等于排空。** 可重复调用路径需保持安全（实现按停机后再 join/清空）。
- 停机后 `RegistCoroutineTask`：非 noexcept 抛 `std::runtime_error("scheduler stopped: coroutine task rejected")`。
- 依据：`Scheduler.cc` `Stop` / `RegistCoroutineTask`；README「三之二」；`unit_test/Test_scheduler_api.cc`

### `Scheduler::RegistCoroutineTask`

- 签名：
  - `void RegistCoroutineTask(const CoroutineCallback& handle, const char* desc = nullptr);`
  - `void RegistCoroutineTask(const CoroutineCallback& handle, bool& succ) noexcept;`
- 前置：调度器运行中。超过 `m_cfg_max_coroutine` 会抛异常（配置注释）。
- `desc`：#276 起写入协程，诊断可读回。
- 依据：`Scheduler.cc`；`syntax/_CoHelper.hpp`

---

## 2. 协程注册宏

头文件：`bbt/coroutine/syntax/SyntaxMacro.hpp`。宏只薄封装，不另起状态机。

### `bbtco`

- 映射：`_CoHelper()-` → `RegistCoroutineTask(func, desc=nullptr)`
- 用法：`bbtco [](){ ... };` 或 `bbtco [&](){ ... };`
- 执行：detached，任意时刻任意 worker。注册后**不保证顺序**。
- 必须在 `Start` 之后。停机后抛 `runtime_error`。

### `bbtco_desc(desc)`

- 映射：`_CoHelper(desc)-`，`desc` 为 `const char*`。
- 用法：`bbtco_desc("worker") [](){};`
- 说明：与 `bbtco` 相同，描述落库。`SyntaxMacro.hpp` 顶部旧注释「当前丢掉 desc」已过时，以 `_CoHelper` 与 README 为准。

### `bbtco_ref`

- 定义：`#define bbtco_ref bbtco [&]()`
- 用法：`bbtco_ref { ... };`（注意是块，不是 `[]()`）。
- 捕获：默认引用捕获，生命周期由调用方保证。

### `bbtco_noexcept(succ)`

- 映射：`_CoHelper(succ)+` → `RegistCoroutineTask(func, *succ) noexcept` 路径
- 用法：`bool succ; bbtco_noexcept(&succ) [](){};`
- 停机或注册失败：`succ=false`，不抛。成功：`succ=true`。
- 注意：宏参数名是 `succ`，传入的是 `bool*`。

---

## 3. 让出、睡眠、查询

### `bbtco_yield`

- 映射：当前 TLS 协程 `YieldAndPushGCoQueue()`。无当前协程则空操作。
- 必须在协程内才有意义。让出后进入就绪队列，稍后可能在其他 worker 恢复。

### `bbtco_sleep(ms)`

- 映射：`detail::Hook_Sleep(ms)`
- 协程内：挂起毫秒后唤醒，不占死 worker。
- 非协程：走 Hook 的非协程分支（原生睡眠）。优先在协程内使用。
- 依据：`SyntaxMacro.hpp`；`Hook.hpp` `Hook_Sleep`

### `GetLocalCoroutineId`

- 头文件：`bbt/coroutine/coroutine.hpp`
- 签名：`detail::CoroutineId GetLocalCoroutineId();`
- 返回：当前协程 id；非协程上下文（含 main）为 `0`。
- 依据：`coroutine.cc`

### `GetLocalCoroutineStackSize`

- 签名：`size_t GetLocalCoroutineStackSize();`
- 返回：当前协程栈大小；非协程为 `0`。

### 取消（协作式）

- 头文件：`bbt/coroutine/detail/Coroutine.hpp`
- 签名：`void RequestCancel() noexcept;` / `bool IsCancelRequested() const noexcept;`
- 取当前协程：`g_bbt_tls_coroutine_co`（`Define.hpp`）。非协程为 `nullptr`。
- 语义：不强杀。运行中代码在检查点 / 等待返回后自行退出，RAII 照常。
- 依据：契约 §2；README「三之二」；#266

### 诊断

- `const std::string& GetDescription() const noexcept` — `bbtco_desc` 写入的名字，未命名空串。
- `int GetWaitInfo(CoroutineWaitInfo& out) const noexcept` — **仅协程自身、同 Processer 线程**可读（头文件时序约束）。

---

## 4. Chan

头文件：`bbt/coroutine/sync/Chan.hpp`，实现 `__TChan.hpp`。

### `sync::Chan<TItem, Max>`

- `Max > 0`：有缓冲。`Max == 0`：特例化无缓冲（写等到读端取走）。
- 构造：`sync::Chan<int, 8>{}` 或 `bbt::coroutine::Chan<int, 8>()`（`coroutine.hpp` 工厂，返回 `SPtr`）。
- 不可拷贝。

| 方法 | 挂起 | 返回 |
|------|------|------|
| `int Write(const T&)` | 缓冲满（或无缓冲未配对）时挂起 | `0` 成功，`-1` 已关闭，`-2` 错误 |
| `int Read(T&)` | 空时挂起 | 同上 |
| `int ReadAll(vector<T>&)` | 会挂起 | 同上 |
| `int TryWrite(const T&)` | 否 | 同上 |
| `int TryRead(T&)` | 否 | 同上 |
| `int TryWrite(const T&, int timeout)` | 最多 `timeout` ms | 另加 `1` 超时 |
| `int TryRead(T&, int timeout)` | 最多 `timeout` ms | 另加 `1` 超时 |
| `void Close()` | 否 | 关闭后读失败 |
| `bool IsClosed()` | 否 | |
| `size_t size() const` | 否 | 当前缓冲数量 |
| `size_t capacity() const` | 否 | `Max` |

- 运算符：`bool operator<<(Chan&, const T&)` / `operator>>` 以及 `shared_ptr` 重载，等价 `Write/Read == 0`。
- 析构：未关闭则 `Close()`。
- `Add*Watcher`：给 `CoSelect` 用，用户代码一般不要碰。
- 必须在协程内调用会挂起的读写。
- 依据：`Chan.hpp` 注释；`__TChan.hpp`；`example/chan.cc`；`openspec/specs/chan-*`

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

- `Run()` **必须在协程内**。返回命中 Case 的注册下标；`-1` = timeout / `Default` / 唤醒后全未就绪（合法竞争，可外层循环）。
- `CaseTimeout(ms)` 与 `Default()` 同时出现时 **Default 优先**（不挂起）。
- `Default()`：全部未就绪立即 `-1`。
- **仅 `Max > 0`**。`Chan<T,0>` `static_assert`。
- 引用重载：Chan 与 `out` 必须活到 `Run()` 返回。`SPtr` 重载在该表达式内保活，跨语句需自行持有。
- 依据：`CoSelect.hpp` 文件头注释；`unit_test/Test_coselect.cc`

---

## 6. CoMutex / 读写锁 / RAII

### `sync::CoMutex`

- 头文件：`bbt/coroutine/sync/CoMutex.hpp`
- 创建：`bbtco_make_comutex()` → `CoMutex::Create()` → `SPtr`
- 必须在协程内加锁等待。

| 方法 | 返回 |
|------|------|
| `void Lock()` | 阻塞直到获得；同协程重入走底层 assert |
| `void UnLock()` | |
| `int TryLock()` | `0` 成功，`-1` 未获得 |
| `int TryLock(int ms)` | `0` 成功，`1` 超时，`-1` 错误 |

### `sync::CoLockGuard<Mutex>` / `CoUniqueLock<Mutex>`

- 头文件：`bbt/coroutine/sync/CoLockGuard.hpp`
- `CoLockGuard`：构造 `Lock()`，析构 `UnLock()` noexcept。持有 `shared_ptr`。不可拷贝移动。
- `CoUniqueLock`：类似 `std::unique_lock`（`defer_lock` / `try_to_lock` / `adopt_lock` / `try_lock_for(ms)` / 移动）。
- `try_lock()` / `try_lock_for` 返回 `bool`（内部 `TryLock==0`）。

### `sync::CoRWMutex`

- 头文件：`bbt/coroutine/sync/CoRWMutex.hpp`
- 创建：`bbtco_make_corwmutex()`
- 写优先：有等待写锁时新读者会挂起（`m_has_wait_wlock`）。
- `RLock` / `WLock` 成功返回 `0`。不允许持读锁再升级写锁（assert）。
- `TryRLock` / `TryWLock`：`0` 成功，`-1` would block。带 `ms`：`0` / `1` / `-1`。
- RAII：`CoReadLock` / `CoWriteLock`（同一头文件）。

依据：各头文件；openspec `rwmutex-*`；`example/comutex.cc`

---

## 7. CoCond

- 头文件：`bbt/coroutine/sync/CoCond.hpp`
- 创建：`bbtco_make_cocond()` → `SPtr`
- 必须在协程内 `Wait`。

| 方法 | 返回（来自 `CoWaiter`） |
|------|------------------------|
| `int Wait()` | `0` 被唤醒，`-1` 失败 |
| `int WaitFor(int ms)` | `0` 唤醒，`1` 超时，`-1` 失败（超时路径经 `WaitWithTimeoutAndCallback`） |
| `int NotifyOne()` | `0` 唤醒了一个，`-1` 当时无等待者 |
| `void NotifyAll()` | 唤醒当前队列全部 |

析构时若队列非空会打「coroutine maybe have been lost」调试日志。

依据：`CoCond.cc`；`CoWaiter.hpp`；`example/cocond.cc`

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
| `void Release()` | 停止 `Submit`；等运行中协程退出；**取消式排空**未执行任务；带 future 的以 `broken_promise` 兑现。幂等 |

池内任务挂起会占用那条池协程。worker 异常被隔离，不打爆调度器。

依据：`CoPool.hpp` / `CoPool.cc`；README「三之二」；#281

---

## 9. 事件等待

### `bbtco_wait_for(fd, event, ms)`

- 头文件：`SyntaxMacro.hpp`；实现 `_WaitForHelper.cc`
- 前置：必须在协程内，否则 assert。
- 行为：构造即挂起，直到 fd 事件或超时。失败抛 `std::runtime_error("wait for failed!")`。
- `event`：`bbtco_emev_readable` / `writeable` / `timeout` / `close` / `finalize` / `persist`（来自 `bbt::pollevent::EventOpt`）。
- 对照：`example/waitfor.cc`

### `bbtco_ev_*`

注册「事件完成时再起一个协程」：

| 宏 | 含义 |
|----|------|
| `bbtco_ev_r(fd)` / `bbtco_ev_w(fd)` | 可读 / 可写，无超时 |
| `bbtco_ev_rc` / `bbtco_ev_wc` | 带 finalize |
| `bbtco_ev_rt(fd, timeout_ms)` / `bbtco_ev_wt` | 带超时 |
| `bbtco_ev_t(timeout_ms)` | 纯超时，`fd=-1` |

`with_copool` 变体把回调丢进指定 `CoPool`。第一版不展开，见 `SyntaxMacro.hpp`。

---

## 10. Defer

### `bbtco_defer`

- 映射：`detail::Defer`，与 C++ 析构顺序一致（后声明的先执行）。
- 用法：`bbtco_defer { cleanup(); };` 在协程内。
- 对照：`example/coroutine.cc` `Defer()`

---

## 11. Hook / I/O 边界

头文件：`bbt/coroutine/detail/Hook.hpp`（随 `coroutine.hpp` 引入）。

协程上下文拦截并转为协程等待（非协程直通原生），包括：

- 套接字：`socket` / `connect` / `accept` / `accept4` / `close` / `read` / `write` / `send` / `recv` / `sendto` / `recvfrom` / `sendmsg` / `recvmsg` / `readv` / `writev`
- 时间：`sleep` / `usleep` / `nanosleep` / `clock_nanosleep`
- 多路复用：`poll` / `select` / `pselect`
- DNS：`getaddrinfo` / `getnameinfo` / `gethostbyname` / `gethostbyaddr`

当前实现约束（README「三之二」，#260/#261/#262）：

- 不要求传入 blocking fd；协程 IO 期间临时 `O_NONBLOCK`，返回时恢复原 flags。
- `MSG_DONTWAIT` 直通原生。
- `SO_RCVTIMEO` / `SO_SNDTIMEO` 由协程 deadline 做成有界返回 `-1/EAGAIN`，不变成无限等待。
- 等待中 fd 被 close：唤醒后重试 syscall，得到 `EBADF`。
- 多线程共享同一 fd 并发 IO 的 flags 竞态：**契约排除项**。

逐调用 errno 矩阵以测试为准，不在本页复制。

---

## 12. 配置

- 头文件：`bbt/coroutine/detail/GlobalConfig.hpp`
- 访问：`g_bbt_coroutine_config`
- **非线程安全，`Start` 前设置。**

| 字段 | 默认 | 说明 |
|------|------|------|
| `m_cfg_max_coroutine` | `65535` | 超过再注册抛异常 |
| `m_cfg_static_coroutine` | `0` | 启动预创建数量 |
| `m_cfg_stack_size` | `1024 * 12` | 12 KiB。不是 README 旧文的 2MB |
| `m_cfg_stack_protect` | `true` | 保护页，溢出 SIGSEGV |
| `m_cfg_static_thread_num` | `0` → `hardware_concurrency()` | worker 数 |
| `m_cfg_scan_interval_ms` | `1` | 调度扫描间隔 |
| `m_ext_coevent_exception_callback` | `nullptr` | detached 异常回调；回调自身不得再抛给运行时 |
| `m_unhandled_exception_count` | `0` | 无回调时计数 |
| `m_cfg_worker_stall_warn_ms` | `0` 关 | worker 单次执行超过该毫秒告警 |
| `m_ext_worker_stall_callback` | 默认 stderr | 调度线程调用，勿阻塞 |

`m_cfg_processer_*`、栈池参数多为内部调优，第一版不展开。

---

## 13. 与契约的已知文档差

- 契约是**目标**；本页描述**当前用户可调用行为**。
- `SyntaxMacro.hpp` 文件头仍写「bbtco_desc 丢掉 desc」，实现已落库（#276）。以实现与 README 为准。
- README「默认 2MB 栈」与 `m_cfg_stack_size` 不一致，以 `GlobalConfig.hpp` 为准。
