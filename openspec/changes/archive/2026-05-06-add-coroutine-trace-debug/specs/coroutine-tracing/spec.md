## ADDED Requirements

### Requirement: Runtime provides coroutine trace mode with bounded per-co history
runtime SHALL 提供独立于聚合 `Profiler` 的 coroutine trace 模式，用于记录单个 coroutine 的元数据与最近调度事件。该 trace 模式 MUST 使用有界存储保存每个被追踪 coroutine 的最近事件，而不是无限增长的历史记录。

#### Scenario: Trace mode records recent events for a selected coroutine
- **WHEN** runtime 以启用 trace 的构建运行，且某个 coroutine 命中 trace 过滤条件
- **THEN** runtime 为该 coroutine 保存元数据，并记录其最近的调度事件序列

#### Scenario: Trace history remains bounded
- **WHEN** 某个被追踪 coroutine 产生的事件数量超过已配置或文档化的保留上限
- **THEN** runtime 只保留最近的事件窗口，并丢弃更早的事件而不导致存储无限增长

### Requirement: Trace mode supports coroutine-level filtering
runtime SHALL 在记录详细 trace 前先应用过滤策略。过滤器 MUST 至少支持按 `CoroutineId`、描述信息或描述前缀选择目标 coroutine，并允许未命中的 coroutine 不进入详细 trace 路径。

#### Scenario: Coroutine does not match trace filter
- **WHEN** runtime 启用 trace，但某个 coroutine 未命中过滤条件
- **THEN** runtime 不为该 coroutine 记录完整的详细事件历史

#### Scenario: Coroutine matches description-based filter
- **WHEN** trace 过滤器配置为匹配某个描述或描述前缀，且新创建的 coroutine 满足该条件
- **THEN** runtime 将该 coroutine 标记为被追踪，并从创建后开始记录其详细事件

### Requirement: Trace queries expose per-coroutine diagnostics
runtime SHALL 提供查询接口或等价的诊断输出，使用户和维护者能够按 `CoroutineId` 或描述信息查看活跃 coroutine 的元数据与最近事件。

#### Scenario: Query by coroutine id
- **WHEN** 用户或调试工具按某个已知 `CoroutineId` 查询 trace 数据
- **THEN** runtime 返回该 coroutine 的元数据与最近事件，或返回文档化的“未找到”结果

#### Scenario: Query active coroutines by description
- **WHEN** 用户或调试工具按描述信息筛选活跃 coroutine
- **THEN** runtime 返回所有匹配项的摘要，并允许进一步查看各自的最近事件
