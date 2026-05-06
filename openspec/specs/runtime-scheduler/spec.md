## ADDED Requirements

### Requirement: Scheduler and Processer responsibilities are explicitly separated
runtime SHALL 将 Scheduler 定义为全局协调组件，并将 Processer 定义为线程绑定的执行组件。Scheduler MUST 负责全局激活、事件泵协同、负载分配以及停机时序。Processer MUST 负责本地 runnable 队列、协程执行以及本地空闲/挂起状态迁移。

#### Scenario: Global activation is routed through Scheduler
- **WHEN** 协程因新注册或事件重新激活而变为 runnable
- **THEN** runtime 在将该协程交给某个 Processer 之前，先通过 Scheduler 持有的激活路径完成投递

#### Scenario: Local execution remains Processer-owned
- **WHEN** 某个 runnable 协程被选中并准备在 worker 线程上执行
- **THEN** 实际的 resume、yield 返回以及本地队列消费都发生在所属 Processer 上，而不是 Scheduler 的事件循环线程

### Requirement: Work distribution and stealing are concurrency-safe
runtime SHALL 保证负载分配与任务偷取使用并发安全的状态迁移。Scheduler 或 Processer 使用的共享计数器、索引和停机标志 MUST NOT 依赖未定义行为或未同步的共享写入。

#### Scenario: Multiple Processers attempt to steal work concurrently
- **WHEN** 两个或以上 Processer 同时进入 work-steal 路径
- **THEN** runtime 保持队列完整性，不重复转移协程所有权，也不破坏共享负载均衡状态

#### Scenario: Scheduler updates balancing state under contention
- **WHEN** Scheduler 在 worker 线程并发运行时选择目标 Processer
- **THEN** 该选择机制在 C++ memory model 下保持定义明确，且不依赖仅使用 volatile 的同步方式

### Requirement: Stop and teardown semantics are deterministic
runtime SHALL 为 Scheduler 与 Processer 的 teardown 定义确定性的 stop 语义。停止 runtime 时 MUST 唤醒挂起中的 worker、一致地清空或终结其拥有的 runnable 状态，并使 runtime 进入明确定义的终态。

#### Scenario: Runtime stops while workers are idle
- **WHEN** 在一个或多个 Processer 因等待任务而挂起时调用 Stop
- **THEN** 每个挂起的 worker 都会被唤醒、观察到 stop 请求，并在不依赖外部轮询循环的前提下退出

#### Scenario: Runtime stops with queued coroutines remaining
- **WHEN** 在全局或本地 runnable 队列仍有协程残留时调用 Stop
- **THEN** runtime 按照文档化的终结策略处理这些协程，使其要么被安全释放，要么按设计被显式保留，但绝不能静默泄漏

### Requirement: Scheduler timing behavior is observable and configurable
runtime SHALL 通过配置项和可观测接口暴露与调度时序相关的行为，使 scan interval、idle wait、steal threshold 以及单个 coroutine 的调度原因都能够被验证与调优。启用 diagnostics、profiling 或 trace 时，runtime MUST 能解释活跃 coroutine 的排队、resume、yield、事件唤醒与迁移路径。

#### Scenario: Configuration changes scheduling cadence
- **WHEN** 在启动前通过 runtime 配置修改 scan interval 或 worker idle interval
- **THEN** runtime 在执行期间一致地应用新的时序节奏

#### Scenario: Profiling or diagnostics inspect scheduling behavior
- **WHEN** 启用 diagnostics、profiling 或 trace
- **THEN** runtime 能报告足够的调度信息，用于解释队列活跃度、steal 次数、空闲时间、执行吞吐以及某个 coroutine 最近一次为何被调度或挂起

#### Scenario: Work stealing affects a traced coroutine
- **WHEN** 某个命中 trace 过滤条件的 coroutine 因 work stealing 或重新分配而在不同 `Processer` 之间迁移
- **THEN** runtime 会把这次迁移记录为可查询的调度事件，并更新该 coroutine 最近关联的 `Processer` 信息
