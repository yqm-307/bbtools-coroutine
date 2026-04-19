## ADDED Requirements

### Requirement: Public runtime API declares startup and thread-context preconditions
runtime SHALL 为通过 `coroutine.hpp` 和语法辅助宏暴露的公共 API 定义所需的启动顺序以及线程上下文前置条件。凡是要求活跃 Scheduler 或启用协程线程的 API，都 MUST 文档化并强制这些前置条件。

#### Scenario: User submits coroutine before scheduler startup
- **WHEN** 用户代码在 runtime 启动前尝试注册协程或运行依赖协程的行为
- **THEN** runtime 通过文档化的失败路径、断言或不支持状态返回，而不是静默进入未定义行为

#### Scenario: User queries coroutine-local state outside coroutine context
- **WHEN** 在非活跃协程上下文中查询本地协程 id、栈大小等公共辅助信息
- **THEN** API 一致地返回文档化的空状态结果

### Requirement: Syntax helpers preserve runtime semantics without hiding failure modes
runtime SHALL 保证 `bbtco`、`bbtco_sleep`、`bbtco_yield`、`bbtco_wait_for` 以及事件辅助宏在行为上与底层 runtime API 保持同样的语义保证，而不会掩盖失败模式。

#### Scenario: Yield helper resumes the coroutine later through the scheduler
- **WHEN** 某个协程调用 yield 语法辅助宏
- **THEN** 该协程会通过 runtime scheduler 重新进入 runnable 路径，而不是绕过正常激活流程

#### Scenario: Event helper registers follow-up execution
- **WHEN** 使用事件辅助器把用户工作绑定到某个 fd 或 timeout 事件
- **THEN** 该辅助器会按照文档化语义，通过新注册的协程或选定的 CoPool 来安排回调执行

### Requirement: CoPool defines task submission, wakeup, and release semantics
runtime SHALL 定义 CoPool 在 worker 创建、任务提交、唤醒策略、monitor 行为和 release 语义上的行为。不同 `Submit` 重载在未被显式另行说明时，MUST 保持等价的外部可见调度行为。

#### Scenario: Task is submitted through any supported overload
- **WHEN** 用户代码通过任意公共 `Submit` 重载提交任务
- **THEN** 该任务在相同的唤醒保证下对 worker 协程可见，而不依赖某个重载特有的轮询副作用

#### Scenario: CoPool is released while tasks remain pending
- **WHEN** 在仍有待处理任务或 worker 已阻塞时调用 `Release`
- **THEN** runtime 按照文档化的 drain 或 shutdown 策略结束该池，而不会让 worker 协程永久阻塞

### Requirement: Runtime exposes diagnostics useful for API consumers
runtime SHALL 提供足够的对外诊断信息、断言或 profiling hooks，使用户和维护者能够识别启动顺序、线程上下文或池 shutdown 行为上的误用。

#### Scenario: API misuse occurs during debug or diagnostic mode
- **WHEN** 用户代码在支持诊断的构建或模式下违反公共 API 前置条件
- **THEN** runtime 会发出可操作的断言、日志或 profile 信号，明确指出误用所属领域