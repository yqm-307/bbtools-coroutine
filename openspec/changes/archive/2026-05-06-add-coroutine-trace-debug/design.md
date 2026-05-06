## Context

当前 runtime 已存在三类与调试相关的机制：

- `PROFILE`：输出全局统计与 `Processer` 汇总信息；
- `DEBUG_INFO`：输出调试日志；
- `STRINGENT_DEBUG`：做强约束断言，检查重复 resume/yield、事件重复注册等一致性问题。

这些机制能够帮助发现“系统整体是否异常”，但很难回答“某个 coroutine 在什么时候因什么原因被挂起、被谁重新调度、是否发生过 steal 或迁移”。同时，`bbtco_desc(...)` 当前仅是可读性别名，没有把描述信息落入运行时对象，导致按协程追踪时缺少稳定元数据。

本变更是跨 `Coroutine`、`Scheduler`、`Processer`、`CoPollEvent`、语法辅助宏与调试输出路径的横切改动，且涉及运行期开销控制，因此需要在实现前先明确新的观测模型与职责边界。

## Goals / Non-Goals

**Goals:**

- 新增独立于 `Profiler` 的 `Trace` 模块，负责单个 coroutine 的详细观测。
- 为 `Coroutine` 增加稳定的元数据模型，支持记录 `id`、`desc`、创建时间、最近状态、最近调度原因和最近执行 `Processer`。
- 提供编译期开关，允许在需要时启用 trace，而不改变默认构建语义。
- 支持 trace 过滤，只对命中的 coroutine 记录详细事件，以控制性能与内存开销。
- 提供基础查询能力，至少支持按 `CoroutineId`、描述信息查看活跃 coroutine 及其最近事件。

**Non-Goals:**

- 不构建图形化 UI、远程可视化或外部 tracing backend。
- 不要求首版保留所有 coroutine 的完整历史；只要求有界的最近事件窗口。
- 不改变现有协程调度算法、优先级语义或 work stealing 策略本身。
- 不用 `Trace` 取代 `Profiler` 的聚合统计职责。

## Decisions

### 1. 保留 `Profiler`，新增独立 `Trace` 模块

`Profiler` 继续承担聚合统计、吞吐观测和 `Processer` 级汇总；`Trace` 专注于单个 coroutine 的元数据、时间线和查询接口。

这样可以避免把“低频详细取证”和“高层全局汇总”耦合在同一个模块中，也能让编译开关语义更清晰：

- `PROFILE`：聚合统计
- `DEBUG_INFO`：调试打印
- `STRINGENT_DEBUG`：一致性断言
- `CO_TRACE`（名称以实现时为准）：单协程追踪

备选方案是直接重构 `Profiler` 使其同时承担 per-co trace。该方案会混淆职责，并使现有 profile 输出与新 trace 筛选策略相互牵制，因此不采用。

### 2. 引入 `CoroutineMeta` 作为运行时一等数据模型

每个 `Coroutine` 拥有稳定的 `CoroutineMeta`，至少包含：

- `CoroutineId`
- `desc`
- 创建时间戳
- 当前状态
- 最近一次调度原因
- 最近一次关联的 `ProcesserId`
- 是否启用详细追踪

需要暴露稳定的读取路径，使查询接口和调试输出都从同一份元数据读取，而不是临时拼接状态。

备选方案是把这些信息分散存入 `Trace` 内部 map。该方案会导致 `Coroutine` 本体与调试信息脱节，也不利于让 `bbtco_desc(...)`、公共 API 与查询接口共享同一语义，因此不采用。

### 3. `bbtco_desc(...)` 必须真正写入元数据

当前 `bbtco_desc(...)` 只是 `bbtco` 的别名，无法承载诊断语义。本设计要求把描述信息通过注册入口传入 `CoroutineMeta.desc`，并保持未提供描述时的默认行为可预测，例如空字符串或实现定义的默认标签。

备选方案是保留宏不变，只在后续通过外部 map 绑定描述。该方案无法保证描述与协程对象生命周期一致，也会增加误用风险，因此不采用。

### 4. 详细 trace 采用“先筛选、后记录”的策略

详细事件记录必须由过滤器决定，避免对全部 coroutine 都记录细粒度时间线。首版过滤维度至少包括：

- `CoroutineId`
- `desc`
- 描述前缀或标签

过滤命中的 coroutine 才进入详细记录路径；未命中的 coroutine 仅维持必要的公共状态，不产生完整事件序列。

备选方案是全量记录后再查询。该方案实现简单，但与“控制性能开销”的目标冲突，因此不采用。

### 5. 每个 coroutine 的事件历史使用有界 ring buffer

`Trace` 为每个被追踪的 coroutine 保留最近 N 条事件，事件类型至少覆盖：

- create
- enqueue
- resume
- yield
- event register
- event trigger
- steal / migrate
- finish
- exception / teardown

使用有界 ring buffer 可以确保内存成本与 coroutine 数量、保留长度成线性关系，且不会因为长时间运行而无限增长。

备选方案是保留完整历史。该方案更利于离线分析，但不满足当前轻量调试目标，因此不采用。

### 6. 查询接口优先满足调试排障，而不是通用数据导出

首版查询接口至少应支持：

- 按 `CoroutineId` 获取元数据与最近事件；
- 按 `desc` 或标签列出匹配的活跃 coroutine；
- 输出活跃 coroutine 摘要，便于结合现有 `DebugPrint` 使用。

接口形态可以是 runtime 内部诊断 API、调试 dump 或两者结合，但必须保证输出基于同一份 `Trace` 数据模型。

## Risks / Trade-offs

- **[详细 trace 仍可能带来热点路径开销]** → 通过编译期开关与过滤器双重控制，并优先让未命中 coroutine 走轻量路径。
- **[元数据写入扩大公共 API 表面积]** → 仅先固化最小必要字段，避免首版暴露过多可变属性。
- **[事件语义命名不一致会降低查询可读性]** → 在实现前统一事件枚举与调度原因枚举，并让所有记录点复用同一套命名。
- **[调试数据与生命周期不同步会导致误判]** → 将核心状态字段放在 `CoroutineMeta`，并让 teardown 路径显式记录最终事件。
- **[`bbtco_desc(...)` 行为变化影响现有用户心智]** → 保持其原有注册行为不变，只新增“描述会被保存并可查询”的语义，不引入新的失败模式。
