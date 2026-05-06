## MODIFIED Requirements

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
