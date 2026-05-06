## MODIFIED Requirements

### Requirement: Runtime exposes diagnostics useful for API consumers
runtime SHALL 提供足够的对外诊断信息、断言、profiling hooks 与 trace 查询入口，使用户和维护者能够识别启动顺序、线程上下文、池 shutdown 行为以及单个 coroutine 的调度问题。支持描述信息的公共 API 或语法辅助宏 MUST 将该描述稳定地写入 coroutine 元数据，以便后续按实例查询与筛选。

#### Scenario: API misuse occurs during debug or diagnostic mode
- **WHEN** 用户代码在支持诊断的构建或模式下违反公共 API 前置条件
- **THEN** runtime 会发出可操作的断言、日志、profile 信号或 trace 诊断，明确指出误用所属领域

#### Scenario: Syntax helper attaches coroutine description
- **WHEN** 用户通过 `bbtco_desc(...)` 或等价公共入口注册新的 coroutine
- **THEN** runtime 会把提供的描述写入该 coroutine 的元数据，并使其可被 trace 过滤与查询接口读取
