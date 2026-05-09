## 背景

阶段 1 的 `extract-runtime-wait-protocol` 用来冻结等待协议与桥接稳定面，阶段 2 的 `align-runtime-lifecycle-and-scheduler` 用来冻结 runtime 内核的生命周期和 stop 语义。只有这两层都稳定之后，`sync-primitives` 才适合进入系统迁移阶段。

当前 `sync` 层的问题已经足够明确：

- `CoCond` 把栈上 waiter 地址直接入队，notify / timeout / destroy 竞争时生命周期收口不统一。
- `CoMutex` 当前直接拼装 custom wait，`TryLock(int ms)` 的 timeout 语义还没有真正落在统一桥接面上。
- `Chan` 同时管理资源状态与等待状态，失败路径和 close 路径会交叉影响内部 reading / writer 状态。
- `CoRWMutex` 的公平性策略隐含在局部实现里，读写 waiter 的超时、取消和关闭语义尚未与其他原语统一。

这些问题不能再通过每个类局部打补丁来解决。阶段 3 的目标是把 `sync-primitives` 整体迁移到前两个阶段已经冻结的稳定面上，并把各原语的资源语义与等待语义明确分离。

## 变更内容

- 引入阶段 3 的独立 change，迁移 `CoCond`、`CoMutex`、`Chan` 与 `CoRWMutex` 到共享等待协议层。
- 让 `sync-primitives` spec 中关于 waiter 生命周期、close / timeout 语义、读写公平性和状态恢复的要求具备明确实现落点。
- 固定 `CoCond`、`CoMutex`、`Chan`、`CoRWMutex` 的迁移顺序，避免最复杂对象先行导致协议和资源语义混改。
- 要求本阶段只消费阶段 1 的等待协议稳定面和阶段 2 的 runtime 内核稳定面，而不回头扩张或重写它们。
- 要求本阶段同步交付 sync 专项测试，覆盖单元、竞争、回归和压力场景。

## 为什么现在做

本 change 位于 `align-runtime-lifecycle-and-scheduler` 之后，原因是：

- 阶段 1 决定等待怎么表达，阶段 2 决定 coroutine / event / runnable 如何安全结束，阶段 3 才能在这两层稳定前提上迁移同步原语。
- 如果跳过阶段 2 就迁移 `sync`，后续 runtime 核心一旦调整 stop 协议或生命周期边界，`sync` 仍会出现明显返工。
- 只有等 custom wait、timeout-aware custom wait 与 fd wait 的基础完成语义都被冻结后，`sync` 才能把重点放在资源条件、close 语义和公平性策略上。

因此，这个 change 的角色不是“优化几个锁和 channel”，而是把 `sync-primitives` 从 runtime 实现细节上剥离出来的中层收口阶段。

## 本阶段明确不做什么

- 不在本 change 中重新定义共享等待协议、bridge surface 或 runtime 核心 stop 语义。
- 不在本 change 中收口 Hook、`CoPool`、`coroutine.hpp` 或 `syntax/*` 的最终外部行为。
- 不要求在本阶段直接处理 `CoPool` 的 worker 唤醒或 release 契约漂移。
- 不要求在本阶段引入新的 public API。

## 能力域

### 修改的 Capabilities
- `sync-primitives`

### 新增 Capabilities

- 无。

## 影响范围

- 受影响代码：`bbt/coroutine/sync/CoWaiter.*`、`bbt/coroutine/sync/CoCond.*`、`bbt/coroutine/sync/CoMutex.*`、`bbt/coroutine/sync/CoRWMutex.*`、`bbt/coroutine/sync/Chan.*`、`bbt/coroutine/sync/__TChan.hpp`
- 受影响测试：`unit_test/Test_cond.cc`、`unit_test/Test_comutex.cc`、`unit_test/Test_chan.cc`、`unit_test/Test_co_rwmutex.cc` 以及阶段 1 产出的协议桥接测试
- 受影响流程：后续 `stabilize-hook-and-copool-semantics` 与 `clean-public-api-and-syntax-surface` 应把本 change 视为 sync 中层稳定面，而不是继续修改其基础等待语义