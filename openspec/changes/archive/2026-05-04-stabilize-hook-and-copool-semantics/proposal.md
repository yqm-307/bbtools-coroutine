## 背景

当阶段 1、2、3 分别冻结等待协议、runtime 核心和 sync 中层后，Hook 与 `CoPool` 才适合进入单独收口阶段。它们当前的问题已经不再是“底层协议不够稳定”，而是中上层行为仍依赖旧副作用和偶然实现。

当前代码里最明显的信号有两类：

- Hook 路径直接依赖 `YieldUntilFdReadable()`、`YieldUntilFdWriteable()`、`YieldUntilTimeout()`，并在部分测试中使用宿主环境假设，例如本机 `127.0.0.1:22`。
- `CoPool` 的两个 `Submit` 重载外部可见语义不一致：一个提交后立即 `NotifyOne()`，另一个只入队；`Release()` 同时依赖 `NotifyAll()`、`g_scheduler->IsRunning()` 和线程上下文语义来决定 drain 行为。

这说明阶段 4 的核心任务不是继续修底层协议，而是把 Hook I/O 和 `CoPool` 的对外语义从旧副作用上剥离出来。

## 变更内容

- 引入阶段 4 的独立 change，收敛 Hook I/O 行为与 `CoPool` 的提交、唤醒、release 语义。
- 让 `event-hooking` spec 中的 fd / timeout / retry 语义在 Hook 实现层具备稳定落点。
- 让 `coroutine-pool-and-api` spec 中关于 `Submit` 等价性、worker 唤醒和 `Release` drain/shutdown 语义具备稳定落点。
- 要求本阶段只消费前 3 个阶段冻结的稳定面，而不回头修改基础等待或 lifecycle 协议。
- 其中阶段 3 的 sync 输入边界已明确冻结为：`CoCond` expired waiter 跳过、`CoMutex` timeout-aware `TryLock`、`Chan` close/timeout/state-restore 语义，以及 `CoRWMutex` writer-preferred + owner 校验规则。

## 能力域

### 修改的 Capabilities
- `event-hooking`
- `coroutine-pool-and-api`

### 新增 Capabilities

- 无。

## 影响范围

- 受影响代码：`bbt/coroutine/detail/Hook.*`、`bbt/coroutine/pool/CoPool.*`、`bbt/coroutine/syntax/_EventHelper.*`、`bbt/coroutine/syntax/_WaitForHelper.*`
- 受影响测试：`unit_test/Test_hook.cc`、`unit_test/Test_coevent.cc`、`unit_test/Test_copool.cc` 以及后续新增的 Hook / CoPool 专项测试
- 受影响流程：后续 `clean-public-api-and-syntax-surface` 应把本 change 视为中上层行为稳定面，而不是继续修改其基础语义

## 已冻结的阶段 3 前置面

阶段 4 不应再修改以下 sync 语义，只能直接消费：

- `CoCond`：统一 waiter 生命周期与 expired waiter 跳过。
- `CoMutex`：owner 校验、timeout-aware `TryLock(int ms)` 与统一 notify 语义。
- `Chan`：buffered / unbuffered close、timeout 与状态恢复。
- `CoRWMutex`：writer-preferred、公平性可观测性、非 owner 读写 unlock 防护与统一 waiter queue 语义。
