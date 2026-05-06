## MODIFIED Requirements

### Requirement: Coroutine state transitions are valid and observable
runtime SHALL 为 runnable、running、suspended 和 final 状态定义合法的协程状态迁移。非法迁移 MUST 能通过断言、诊断信息或测试被检测出来；在启用 trace 或诊断模式时，runtime MUST 能按 coroutine 实例暴露最近一次状态迁移的原因与上下文。

#### Scenario: Coroutine yields during execution
- **WHEN** 一个 running 协程在未等待外部事件时主动 yield
- **THEN** 它会回到 runnable 状态，并可再次被调度，且不会进入非法中间状态

#### Scenario: Coroutine suspends on an awaitable event
- **WHEN** 一个 running 协程因等待 timeout、fd 就绪或自定义唤醒而挂起
- **THEN** 它会迁移到 suspended 状态，并在对应唤醒路径重新激活前不再被调度

#### Scenario: Trace inspects recent lifecycle transitions
- **WHEN** 用户查询某个被追踪 coroutine 的生命周期信息
- **THEN** runtime 返回该 coroutine 最近的状态迁移事件，并指明对应的调度原因、事件原因或 teardown 原因
