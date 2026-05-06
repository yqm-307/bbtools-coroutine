## 1. 编译开关与模块骨架

- [x] 1.1 在构建系统中新增独立的 coroutine trace 编译期开关，并明确其与 `PROFILE`、`DEBUG_INFO`、`STRINGENT_DEBUG` 的宏边界。
- [x] 1.2 新增 trace 模块骨架，拆分过滤、事件记录、存储与查询职责，并接入现有 runtime 单例初始化路径。

## 2. Coroutine 元数据模型

- [x] 2.1 为 `Coroutine` 引入稳定的元数据结构，覆盖 `CoroutineId`、描述信息、创建时间、当前状态、最近调度原因和最近 `ProcesserId`。
- [x] 2.2 改造 coroutine 注册入口与语法辅助宏，使 `bbtco_desc(...)` 和等价入口能够把描述信息写入 coroutine 元数据。

## 3. 调度与事件追踪接入

- [x] 3.1 在 `Scheduler`、`Processer` 与 `Coroutine` 的关键路径接入 trace 事件，至少覆盖 create、enqueue、resume、yield、finish 与 teardown。
- [x] 3.2 在 `CoPollEvent` 与相关等待路径接入事件注册/触发记录，并让 trace 能关联 coroutine 的挂起与恢复原因。
- [x] 3.3 在 work stealing 或跨 `Processer` 迁移路径记录可查询的迁移事件，并同步更新 coroutine 最近关联的 `Processer` 信息。

## 4. 过滤与查询能力

- [x] 4.1 实现按 `CoroutineId`、描述信息或描述前缀筛选追踪目标的过滤器，并保证未命中对象不会进入完整详细记录路径。
- [x] 4.2 为被追踪 coroutine 实现有界 ring buffer 事件历史，并提供按 `CoroutineId` 与描述信息查询活跃 coroutine 和最近事件的接口或诊断输出。

## 5. 验证与示例

- [x] 5.1 为 trace 开关、元数据写入、过滤策略与最近事件查询补充单测或 debug 用例。
- [x] 5.2 更新与调试相关的文档或示例，说明如何开启 trace、如何标注 coroutine 描述以及如何查询单个 coroutine 的最近调度轨迹。
