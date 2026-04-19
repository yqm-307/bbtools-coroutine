## 背景

阶段 1 的 `extract-runtime-wait-protocol` 会先冻结等待协议、完成结果和桥接稳定面，但这还不足以让 runtime 核心自然变得稳定。当前 `Scheduler`、`Processer`、`Coroutine` 与 `Context` 之间仍然存在一批直接影响生命周期和停机语义的实现风险，如果不单独收口，后续 `sync`、Hook 和 `CoPool` 仍会建立在不稳定内核之上。

当前代码里至少有四类直接信号表明，阶段 2 必须独立成 change：

- `Scheduler` 把 `m_is_running`、`m_run_status` 声明为 `volatile`，同时 `m_load_idx`、`m_steal_idx` 以普通整数形式跨线程参与负载均衡和任务窃取，这与 `runtime-scheduler` spec 对并发安全状态迁移的要求并不对齐。
- `Scheduler::_LoadBlance2Proc()`、`Scheduler::TryWorkSteal()` 与 `Processer::AddCoroutineTask()` / `Steal()` 共同形成跨线程投递路径，但当前并没有把这些共享索引和 stop 状态收敛到可证明的数据竞争边界上。
- `Scheduler::Stop()` 与 `Processer::Stop()` 在清空全局/本地 runnable 队列时只是把 `Coroutine::Ptr` 置空，没有显式终结或释放残留协程；而 `Coroutine::Ptr` 当前实际上是裸指针，这使 teardown 路径的所有权语义不清晰。
- `Coroutine` 的注释仍在讨论 `shared_ptr` 所有权，但真实实现已经使用裸指针；再加上 `Processer` 只在协程进入 `CO_FINAL` 时 `delete`，说明生命周期模型在实现和注释层面都还没有真正收口。

这些问题不是 `sync` 层可以自行兜底的。只有先把 runtime 核心的生命周期、调度边界和 stop 协议压实，后续阶段才能在稳定内核上推进。

## 变更内容

- 引入阶段 2 的独立 change，收敛 `Scheduler`、`Processer`、`Coroutine` 与 `Context` 的生命周期和并发边界。
- 让 `runtime-scheduler` spec 中的职责分离、并发安全调度和 stop 语义在实现设计上具备明确落点。
- 让 `coroutine-lifecycle` spec 中的所有权模型、状态迁移、异常处理和 teardown 路径具备统一的实现收口方向。
- 要求本阶段只消费阶段 1 已冻结的等待桥接稳定面，而不是重新定义等待完成模型。
- 要求本阶段同步交付 runtime 核心级测试方案，覆盖调度、teardown、状态迁移、异常与生命周期边界。

## 为什么现在做

本 change 的顺序位于 `extract-runtime-wait-protocol` 之后、`migrate-sync-primitives-to-wait-protocol` 之前，原因是：

- 阶段 1 解决的是“等待怎么表达”，阶段 2 解决的是“谁拥有 coroutine / event / runnable 状态，以及这些对象如何安全结束”。
- 如果跳过阶段 2 直接迁移 `sync`，`CoCond`、`CoMutex`、`Chan`、`CoRWMutex` 仍会建立在当前不稳定的 stop、ownership 和激活路径之上。
- 如果在 Hook 或 `CoPool` 阶段再回头修 runtime 核心，前面所有阶段的测试出口都会失去稳定参照。

因此，这个 change 的角色不是“补一些 scheduler bug”，而是方案 1 中承上启下的 runtime 内核收口阶段。

## 本阶段明确不做什么

- 不在本 change 中完成 `sync-primitives` 的系统迁移。
- 不在本 change 中收口 Hook、`CoPool`、`coroutine.hpp` 或 `syntax/*` 的最终对外行为。
- 不重新定义阶段 1 已冻结的等待完成模型、桥接面或协议测试出口。
- 不要求在本阶段一次性确定所有内部数据结构选型，但要求先冻结生命周期与状态迁移规则。

## 能力域

### 修改的 Capabilities
- `runtime-scheduler`
- `coroutine-lifecycle`

### 新增 Capabilities

- 无。

## 影响范围

- 受影响代码：`bbt/coroutine/detail/Scheduler.*`、`bbt/coroutine/detail/Processer.*`、`bbt/coroutine/detail/Coroutine.*`、`bbt/coroutine/detail/Context.*`、`bbt/coroutine/detail/StackPool.*`
- 受影响测试：`unit_test/Test_coroutine.cc`、`unit_test/Test_coroutine_exception.cc`、`unit_test/Test_coroutine_stack.cc` 以及后续新增的 scheduler / lifecycle 专项测试
- 受影响流程：后续 `migrate-sync-primitives-to-wait-protocol`、`stabilize-hook-and-copool-semantics` 与 `clean-public-api-and-syntax-surface` 应把本 change 视为 runtime 内核稳定面，而不是继续修改其基础语义