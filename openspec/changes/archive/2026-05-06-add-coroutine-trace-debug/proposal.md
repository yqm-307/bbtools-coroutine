## Why

当前 runtime 的 `Profiler` 更偏向全局统计与 processer 汇总，难以回答“某个 coroutine 为什么挂起、被谁唤醒、是否发生过迁移”这类调试问题。随着协程数量和调度路径增多，仅依赖 `DEBUG_INFO`、`STRINGENT_DEBUG` 与聚合 profile 已不足以支撑定位复杂调度问题，因此需要引入可按 coroutine 追踪的 trace 能力，并把 coroutine 元数据正式纳入 runtime 模型。

## What Changes

- 新增独立于现有 `Profiler` 的 coroutine trace 模块，用于记录单个 coroutine 的调度事件与最近状态。
- 为 `Coroutine` 引入正式的元数据模型，至少覆盖 `CoroutineId`、描述信息、创建时间、当前状态、最近一次调度原因以及最近关联的 processer。
- 新增编译期开关以启用 trace/debug 观测能力，并明确其与现有 `PROFILE`、`DEBUG_INFO`、`STRINGENT_DEBUG` 的职责边界。
- 为 trace 增加筛选能力，支持仅对被关注的 coroutine 记录详细事件，降低运行期开销。
- 提供按 `CoroutineId`、描述信息等维度查询活跃 coroutine 与最近事件的诊断接口。
- 让 `bbtco_desc(...)` 及相关公共入口真正写入 coroutine 元数据，而不是仅作为可读性占位。

## Capabilities

### New Capabilities
- `coroutine-tracing`: 定义 coroutine trace 模块、按协程筛选追踪、事件时间线与查询接口的对外行为。

### Modified Capabilities
- `coroutine-pool-and-api`: 调整语法辅助宏与公共 API 的诊断语义，使 `bbtco_desc(...)` 等入口能够暴露稳定的 coroutine 元数据。
- `runtime-scheduler`: 扩展调度可观测性要求，使 runtime 能解释单个 coroutine 的调度、挂起、唤醒与迁移原因。
- `coroutine-lifecycle`: 扩展生命周期可观测性要求，使 coroutine 的状态迁移与终结过程可被按实例追踪与查询。

## Impact

- 影响代码主要位于 `bbt/coroutine/detail/Profiler.*`、新增 trace 相关模块、`Coroutine.*`、`Scheduler.*`、`Processer.*`、`CoPollEvent.*` 以及语法辅助宏实现。
- 会新增编译期配置与诊断/查询接口，但不改变默认非 trace 构建下的协程调度语义。
- 需要补充单测或调试用例，覆盖 trace 开关、元数据写入、筛选策略、事件记录与查询行为。
