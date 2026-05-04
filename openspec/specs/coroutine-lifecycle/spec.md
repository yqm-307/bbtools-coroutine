## ADDED Requirements

### Requirement: Coroutine lifecycle has a single ownership model
runtime SHALL 为协程对象在创建、执行、挂起、重新激活、完成和 teardown 全流程中定义单一且明确的所有权模型。生命周期管理 MUST 同时避免重复析构与静默泄漏。

#### Scenario: Coroutine completes normally
- **WHEN** 协程正常执行完其用户回调
- **THEN** runtime 仅一次性地将其迁移到终态，并按文档化的所有权模型释放其所拥有的资源

#### Scenario: Coroutine is still queued during teardown
- **WHEN** runtime teardown 发生时某个协程仍残留在 runnable 队列中
- **THEN** 协程所有权仍保持明确定义，且该协程对象要么被安全终结，要么按策略被显式保留，但绝不能被隐式遗弃

### Requirement: Coroutine state transitions are valid and observable
runtime SHALL 为 runnable、running、suspended 和 final 状态定义合法的协程状态迁移。非法迁移 MUST 能通过断言、诊断信息或测试被检测出来。

#### Scenario: Coroutine yields during execution
- **WHEN** 一个 running 协程在未等待外部事件时主动 yield
- **THEN** 它会回到 runnable 状态，并可再次被调度，且不会进入非法中间状态

#### Scenario: Coroutine suspends on an awaitable event
- **WHEN** 一个 running 协程因等待 timeout、fd 就绪或自定义唤醒而挂起
- **THEN** 它会迁移到 suspended 状态，并在对应唤醒路径重新激活前不再被调度

### Requirement: Yield-with-callback preserves event registration ordering
runtime SHALL 保证用于 suspend 阶段的事件注册回调只会在协程成功 yield 之后执行。该顺序 MUST 防止协程在 suspend 操作完成前被重新激活。

#### Scenario: Event fires immediately after registration
- **WHEN** 协程以带回调的方式 yield，并在回调里注册事件且该事件立即就绪
- **THEN** runtime 不会在 yield 操作完成且协程进入 suspended 状态之前恢复该协程

### Requirement: Exceptions preserve runtime integrity
runtime SHALL 定义协程用户代码或事件回调抛出异常时，对协程状态、事件清理以及外部可见错误报告的影响。

#### Scenario: User coroutine throws an exception
- **WHEN** 协程主体抛出异常
- **THEN** runtime 将该协程标记为终态，取消其拥有的活动等待注册，并将异常路由到已配置的异常处理策略

#### Scenario: Event callback throws an exception
- **WHEN** runtime 调用的事件回调抛出异常
- **THEN** runtime 保持事件对象完整性，使其进入最终事件状态，并按配置的异常策略转发或报告该错误

### Requirement: Stack allocation and reuse are bounded and testable
runtime SHALL 通过 StackPool 配置和 runtime 策略定义有边界的栈分配、复用与收缩行为。栈复用 MUST 与生命周期正确性及清理保证保持兼容。

#### Scenario: Stack pool reaches allocation limit
- **WHEN** 协程创建将超过已配置的 stack pool 最大值
- **THEN** 创建过程通过文档化的错误路径失败，而不是静默超额分配

#### Scenario: Stack pool adjusts during runtime
- **WHEN** StackPool 在稳定执行期间运行 shrink 或 reuse 逻辑
- **THEN** 活跃协程的栈保持不变，只有空闲且可释放的栈参与池调整
