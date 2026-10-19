# bbtools-coroutine API 参考

当前用户可调用行为。签名以头文件为准；本文与头文件冲突时改本文。

时间单位未另写时为**毫秒**。返回码未另写时：**`0` 成功、`-1` 失败、`1` 超时**。

未列入覆盖范围的 `detail/` 成员不当作用户 API。

## 覆盖范围

- **运行时**：`g_scheduler`、`Start` / `LoopOnce` / `IsInitialized`、Processer 扩缩容控制与查询、`SchedulerStartOpt`
- 注册：`bbtco` / `bbtco_desc` / `bbtco_ref` / `bbtco_noexcept`
- 让出：`bbtco_yield`、`bbtco_sleep`
- 查询：`GetLocalCoroutineId`、`GetLocalCoroutineStackSize`
- 同步：`Chan`、`CoSelect`、`CoMutex`、`CoRWMutex`、`CoLockGuard`、`CoUniqueLock`、`CoReadLock`、`CoWriteLock`、`CoCond`
- 池：`CoPool`
- 事件：`bbtco_wait_for`、`bbtco_ev_*`
- Defer：`bbtco_defer`
- Hook 拦截列表与 IO 约束
- 配置：`g_bbt_coroutine_config` 用户字段
- io：`io::GetExecutor`（共享 context 投递句柄，见 §13）

单次内存故障边界：Hook `close` 准备失败不发布 closing/新代际，且不向 C 调用者抛准备异常；一次分配失败后在 syscall 前再准备一次，再次失败返回 `ENOMEM`。PARKED 等待完成时延迟销毁列表的一次扩容失败由原赢家有界重提交，保持唯一唤醒和驱动线程析构所有权。此处不是任意分配点或持续 OOM 的恢复保证。

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
- 前置：一次性启动。成功初始化后重复 `Start`（含并发 `Start`）抛 `std::logic_error`，不重置配置/队列、不新增 worker；`THREAD` 模式 assert `m_sche_thread == nullptr`。
- 选项（`Define.hpp`）：

| 值 | 行为 |
|----|------|
| `SCHE_START_OPT_SCHE_THREAD`（默认） | 后台调度线程，`Start` 在 worker 创建后返回 |
| `SCHE_START_OPT_SCHE_LOOP` | 当前线程阻塞跑调度循环 |
| `SCHE_START_OPT_SCHE_NO_LOOP` | 只创建 worker，由调用方 `LoopOnce` 驱动 |

### `Scheduler::LoopOnce`

- 签名：`void LoopOnce();`
- 前置：仅 `NO_LOOP`。已 `Start(THREAD)` 时 assert。

### `Scheduler::IsInitialized`

- 签名：`bool IsInitialized() const noexcept;`
- 说明：运行时是否已完成必要初始化（runtime/worker/poller）。初始化标志在进入运行循环前发布，完成后恒为 true——没有停机状态，因此不会因业务关闭归零。

### `Scheduler::Stop`（已删除）

- 旧签名 `void Stop();` 与 `void Scheduler::GetRunGeneration()`、`RuntimeGeneration` 一起删除，**不留兼容壳**。运行时是进程寿命对象，不做取消式停机、不排空任务、不 unload（契约 §6）。
- 旧语义（停止接新任务、join worker、回收未执行与 parked 协程、停机后 `RegistCoroutineTask` 抛 `std::runtime_error`）**superseded**，历史报告保持原文并在迁移说明中标注，不作为当前实现依据。
- worker 缩容的协作退出和 reaper join 不属于 Stop/teardown；不展开 parked 栈、不停止全局运行时（目标契约 §6.1–§6.3）。

### Processer 扩缩容控制与查询

头文件：`bbt/coroutine/detail/Scheduler.hpp`；结果及状态类型位于 `Define.hpp`，命名空间均为 `bbt::coroutine::detail`。

| 签名 | 行为 |
|---|---|
| `ProcesserResizeResult SetProcesserNum(size_t num)` | 请求 `[min,max]` 内的活动 worker 数；合法请求（含 same-target）切到 Manual、重置采样；扩容整批发布，缩容协作退役 |
| `ProcesserResizeResult SetProcesserAutoScale(bool enable)` | 显式切换 Auto/Manual 并重置双向采样；不通过并发修改配置字段切换 |
| `size_t GetProcesserNum()` | 当前 active 数，已经扣除 Retiring/Exited，不是物理线程总数；未初始化返回 0 |
| `size_t GetProcesserTargetNum()` | 最近提交的目标数，未初始化为 0 |
| `size_t GetProcesserRetiringNum()` | 已移出 active、尚未发布 Exited 的 worker 数；未初始化返回 0 |
| `size_t GetProcesserExitedUnjoinedNum()` | 已发布 Exited、尚未完成 join 的 worker 数，包含等待 TLS 析构的线程；未初始化返回 0 |
| `ProcesserScalingMode GetProcesserScalingMode()` | 当前 Manual/Auto，未初始化为 Manual |

- `ProcesserLifecycleState{Preparing, Active, Retiring, Exited, Joined}` 区分线程生命周期；不改既有 `ProcesserStatus` 数值或 driver 的结果集合。各 getter 单独同步，不构成同一时刻的联合快照。
- `ProcesserResizeResult`：`Ok` 为扩容/同目标请求已提交；`Pending` 为缩容目标已提交，但退役/物理回收尚未完成；`NotInitialized` / `InvalidTarget` / `DriverBound` / `CreateFailed` / `SelfRetire` / `Busy` 为未接受该目标。拒绝的手动请求不改变 target/mode/采样。扩容准备分配或线程初始化失败返回 `CreateFailed`，不公开半批；缩容准备分配失败或未完成退役总数将超过 32 时返回 `Busy`。
- Bound/Failed/Detaching driver 均 pin 所属 worker，目标低于可保留数量时返回 `DriverBound`。在 worker 上调用时会跳过自身；若必须退役自身才能达到目标，返回 `SelfRetire`，不等待自身。控制可能持锁、创建线程并等待初始化，不是无阻塞接口，建议由外部控制线程调用。
- `Ok`、`Pending`、active 下降及 `PROC_EXIT` 都不是物理 join 收据；观察 retiring 与 exited-unjoined 均归零才能确认已提交退役记录收口。用户 TLS 析构未完成时保持 exited-unjoined，reaper 不为等待它阻塞调度事件驱动。
- THREAD/LOOP 的调度循环推进 auto 与 reaper；NO_LOOP 无隐藏调度线程，auto 采样依赖 `LoopOnce()`，`SetProcesserNum()` 与 `LoopOnce()` 均能推进 reaper。getter 不推进回收；停止调用控制/驱动入口时可能保持 exited-unjoined。单次收割至多 8 条已完成线程退出的记录，不强杀用户代码，不展开 parked 栈。
- `Start` 在一次性启动状态和线程副作用前消费 `ResolveProcesserScalingBounds`；非法配置抛 `std::invalid_argument`，修正后可重试。具体配置见本文配置表，运行时语义见契约 §6.1–§6.3。
- 解析后 `min < max` 时，空闲 active worker 可立即偷取其它 active worker 的本地积压，不等待当前 Resume 超过停顿阈值；手动扩容同样可分担连续短任务。`min == max` 的固定规模保留原偷取阈值。两者都不抢占正在 Resume 的协程，也不保证定向首投的硬亲和。

### `Scheduler::RegistCoroutineTask`

- 签名：
  - `void RegistCoroutineTask(const CoroutineCallback& handle, const char* desc = nullptr);`
  - `void RegistCoroutineTask(const CoroutineCallback& handle, bool& succ) noexcept;`
  - `void RegistCoroutineTask(size_t stack_size, const CoroutineCallback& handle, const char* desc = nullptr);`
  - `void RegistCoroutineTask(size_t stack_size, const CoroutineCallback& handle, bool& succ) noexcept;`
- 前置：运行时已初始化。超过 `m_cfg_max_coroutine` 会抛异常。
- `desc`：写入协程，诊断可读回。空表示未命名。
- 栈大小：带 `stack_size` 的两个重载表示「本协程可用的字节数」，按实测系统页向上对齐后生效（`GetLocalCoroutineStackSize()` 读回的就是对齐后的 U）。不带大小的重载继续取 `m_cfg_stack_size`（出厂 12 KiB）；显式调用**不改写**全局默认，也不改保护开关——保护属性始终取自 `m_cfg_stack_protect`。
- 非法值/失败（在入队前兑现，不发布半成品）：
  - `stack_size == 0` → `std::invalid_argument`
  - 页取整/追加保护页/指针差范围越界（含把旧的 `-1` 之类的负 `int` 转成 `size_t`）→ `std::length_error`
  - 内存不足 → `std::bad_alloc`；建立保护页失败 → 带 `errno` 的 `std::system_error`
  - 无 worker（未 `Start`）等的发布前拒绝 → `std::runtime_error`
  - `bool& succ` 重载不抛：上述任一失败置 `succ = false`，成功置 `true`
- 尺寸在创建后固定：不支持扩栈/换栈，也不按当前默认配置重新计算。
- 兼容边界：过去 `detail::Coroutine::Create(4096, ...)` 的大小被忽略，现在会实际分配 4 KiB（按页对齐）；栈上运行日志、断言或第三方代码时必须给足栈。detail 尺寸/计数类型改为 `size_t`，调用方升级时须重新编译，不混用旧二进制。
- `CoPool` 的固定栈属于其 worker 协程，不能按队列中的任务换栈；需要单独大小的任务使用显式注册入口创建独立协程。

### Processer driver adapter（B1，契约 §11）

头文件：`bbt/coroutine/detail/interface/IProcesserDriver.hpp`（`bbt::coroutine::detail`）。runtime 内部支撑接口，不暴露第三方类型；语义细节以契约 §11 为准。

#### `IProcesserDriver`

- `virtual ProcesserDriverResult OnTick(ProcesserDriverPhase phase) = 0;` — 每调度轮次 `BeforeSchedule`/`AfterSchedule` 各至多一次；只在绑定 P 的 worker 线程、非业务协程上下文调用；须短且不阻塞。返回 `Failed` 或抛异常 → runtime 转 `Failed` 态并记录。
- `virtual ProcesserDriverResult Wait(std::chrono::microseconds timeout) = 0;` — 仅在本地/global/steal 均无 runnable 后调用；`timeout` 上限沿用 `m_cfg_processer_proc_interval_us`（3000us）。driver 必须持久记忆早到 Wake。
- `virtual void Wake() noexcept = 0;` — 线程安全、有界、不等待 worker；允许外线程/并发/嵌套。

#### 枚举与 token

- `ProcesserDriverResult{Ok, Failed}`；`ProcesserDriverState{Unbound, Bound, Failed, Detaching}`；`ProcesserDriverFailure{None, Tick, Wait}`。
- `ProcesserDriverStatus`：`Ok` / `NotInitialized` / `InvalidTarget` / `InvalidDriver` / `Occupied` / `StaleToken` / `SelfDetach`。
- `ProcesserDriverToken{m_processer_id, m_generation}`：解绑凭据，代次单调非零、绑定间不重用。
- `ProcesserDriverSnapshot{m_state, m_token, m_failure, m_exception}`：`Failed` 时可读回失败来源与异常（返回 `Failed` 的异常指针为空）。

#### `Scheduler` adapter 入口

- `std::vector<ProcesserId> GetWorkerProcesserIds()` — 未初始化返回空，不惰性创建 P；调用时快照，不承诺后续绑定/定向投递目标仍有效（动态退役目标契约见 §6.2）。
- `ProcesserDriverStatus RegisterProcesserDriver(ProcesserId, std::shared_ptr<IProcesserDriver>, ProcesserDriverToken&)` — 目标必须是已登记 worker（`GetLocalProcesser()` 惰性 P 与未知 id 一律 `InvalidTarget`）；空 driver `InvalidDriver`；已有归属 `Occupied`；失败不改 `out_token`。
- `ProcesserDriverStatus QueryProcesserDriver(ProcesserId, ProcesserDriverSnapshot&)` — 合法空槽返回 `Ok` + `Unbound` 快照；目标非法 `InvalidTarget`；失败不改 `out_snapshot`。
- `ProcesserDriverStatus UnregisterProcesserDriver(const ProcesserDriverToken&)` — 同代精确校验，旧/伪 token `StaleToken`；目标 worker 线程、受管业务协程、持该代 callback lease 的线程、以及仍处 runtime 通知临界区（`DriverWakeDeferral` 生效）的线程调用一律 `SelfDetach`——否则终止 Wake 会穿透 deferral 在 runtime 锁内执行。返回 `Ok` 前已执行终止 Wake 并等全部在途 Tick/Wait/Wake 退出——这是「runtime 不再调用该代」的确认，不是业务资源已物理关闭的收据。
- `void RegistCoroutineTaskOn(ProcesserId, const CoroutineCallback&, const char* desc = nullptr)` 与带 `stack_size` 重载 — 定向首投：仅入目标 worker 初始队列的偏好，无硬亲和；stealing 与挂起恢复路径不变。未初始化抛 `std::logic_error`，非法目标抛 `std::invalid_argument`。
- 上述可能分配/持锁的入口均非 `noexcept`；全部 driver 回调在整条调用链的 runtime 锁之外执行。

---

## 2. 协程注册宏

头文件：`bbt/coroutine/syntax/SyntaxMacro.hpp`。宏映射到 `RegistCoroutineTask`，不另起状态机。

### `bbtco`

- 映射：`_CoHelper()-` → `RegistCoroutineTask(func, desc=nullptr)`
- 用法：`bbtco [](){ ... };` 或 `bbtco [&](){ ... };`
- 执行：detached，任意时刻任意 worker。不保证顺序。
- 必须在 `Start` 之后。注册仅存在发布前失败（无可用 `Processer` 时抛出），没有停机后拒绝分支；runtime 随进程存在，不做停机。

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
- 只读查询：不创建 Processer、不启动 runtime；TLS 已许可但尚无当前协程时仍为 `0`。每次读取当前执行环境，不缓存 worker 归属。

### `GetLocalCoroutineStackSize`

- 签名：`size_t GetLocalCoroutineStackSize();`
- 返回：当前协程可用的栈字节数（页对齐后的 U，不含保护页）；非协程为 `0`。同一 runtime 内不同协程可返回不同值。
- 与 ID 查询相同，不创建 Processer、不启动 runtime；TLS 未许可时返回 `0`，yield 后重新读取当前协程。

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
| `m_cfg_stack_size` | `1024 * 12` | 省略大小时的默认栈字节数（12 KiB）；显式入口不读它 |
| `m_cfg_stack_protect` | `true` | 保护页（栈自身属性），溢出 `SIGSEGV` |
| `m_cfg_static_coroutine` | `0` | 启动时预创建数量（只对默认 U/保护桶预热，裁剪到 `m_cfg_stackpool_max_alloc_size`） |
| `m_cfg_stackpool_min_alloc_size` | `128` | 定时回收的全池最低保留数（不是每桶下限）；显式清缓存不受它限制 |
| `m_cfg_stackpool_max_alloc_size` | 同 `m_cfg_max_coroutine` | 跨桶「同时借出 + 申请中」的总上限，不是每桶上限或内存字节预算 |
| `m_cfg_static_thread_num` | 构造时 `hardware_concurrency()`，不可用时 `1` | initial worker 数；Start 前显式改成 `0` 非法，不识别用户是否显式设置 |
| `m_cfg_processer_auto_scale` | `false` | 初始自动策略开关；运行期不能并发改配置字段 |
| `m_cfg_processer_min_num` / `m_cfg_processer_max_num` | `0` / `0` | 各自回退 initial；解析后 `1 <= min <= initial <= max <= INT_MAX` |
| `m_cfg_processer_scale_interval_ms` | `100` | `[1, chrono::milliseconds::rep 最大值]` ms |
| `m_cfg_processer_scale_up_samples` / `m_cfg_processer_scale_down_samples` | `3` / `3` | 连续命中次数，各在 `[1, SIZE_MAX]` |
| `m_cfg_processer_scale_step` | `1` | 每拍至多改变的数量，`[1, INT_MAX]`，实际受 min/max 约束 |
| `m_cfg_processer_scale_up_busy_percent` / `m_cfg_processer_scale_down_busy_percent` | `80` / `20` | 各在 `[0,100]` 且 down<=up；不依赖 stall 告警开关 |
| `m_cfg_processer_scale_up_queue_watermark` / `m_cfg_processer_scale_down_queue_watermark` | `1` / `0` | 全局加 active 本地 runnable 近似总数，各在 `[0,SIZE_MAX]`，up>=1 且 down<=up；不作排空证明 |
| `m_cfg_scan_interval_ms` | `1` | 调度扫描间隔 |
| `m_ext_coevent_exception_callback` | `nullptr` | detached 异常回调；回调不得再抛给运行时 |
| `m_unhandled_exception_count` | `0` | 无回调时计数 |
| `m_cfg_worker_stall_warn_ms` | `0` 关 | worker 单次执行超过该毫秒告警 |
| `m_ext_worker_stall_callback` | 默认 stderr | 调度线程调用，勿阻塞 |

`bool GlobalConfig::ResolveProcesserScalingBounds(ProcesserScalingBounds& out_bounds) const noexcept` 校验全部扩缩容字段（auto 关闭也校验），成功返回 true 并写 `m_initial_num/m_min_num/m_max_num`；失败返回 false，输出与原始配置不变，不启动线程。`ProcesserScalingBounds` 与 helper 位于 `GlobalConfig.hpp`，供核心启动路径消费。

扩缩容策略：queue>=up **或** busy>=up 连续命中时扩容；queue<=down **且** busy<=down 连续命中时缩容。双向计数独立，不命中即复位；相等阈值同拍双向命中时仅选扩容，提交后双向复位，每拍至多一批。运行期通过上面的控制入口切换，不直接并发改配置；详见契约 §6.1–§6.3。

其余 `m_cfg_processer_*` 与栈池参数是内部调优，见头文件。

---

## 13. io 共享 executor

### `io::GetExecutor`

- 头文件：`bbt/coroutine/io/IoExecutor.hpp`
- 签名：`boost::asio::any_io_executor GetExecutor();`
- 说明：返回 CoPoller 全局 EventLoop 所持同一 io_context 的 executor（`any_io_executor` 投递句柄），供上层适配层把 Asio 异步操作接入既有协程事件循环。投递的 handler 只由 Scheduler 现有 `PollOnce` 在驱动线程兑现：默认后台模式即 Scheduler 调度线程（S）；`NO_LOOP` 模式即调用 `LoopOnce` 的驱动线程（首个调用 `PollOnce` 的线程）；`SCHE_LOOP` 模式即执行调度循环的当前线程。
- 约束（仅限定共享 executor 的使用方式）：不得为它创建新线程或独立 io_context 驱动；句柄不暴露原始 io_context，无法 stop/restart/run。生命周期由 CoPoller 单例保证，随 runtime 进程寿命存在。
- 边界：本条不禁止上层拥有独立线程与 io_context 的网络 I/O 域（契约 §11 目标架构，尚未实现），独立域不经本句柄投递。头文件注释中「不创建新线程」的措辞按本条限定范围理解，与契约 §11.3 一致。
