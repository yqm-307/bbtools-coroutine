## 背景

当前仓库已经形成了相对清晰的模块边界，但缺少覆盖运行时核心模块的正式 OpenSpec 规范，导致后续重构、修复和测试扩展缺少统一的行为契约。现在补齐按模块拆分的规范，可以把调度、协程生命周期、事件/Hook、同步原语、协程池与公共 API 的职责边界固化下来，并把测试要求前置为变更门槛。

## 变更内容

- 为协程运行时建立按模块拆分的规范集合，分别覆盖调度器、协程对象、事件/Hook、同步原语、协程池与公共 API。
- 为每个模块定义可验证的行为要求，包含正常路径、失败路径、并发竞争、停机/清理与超时语义。
- 新增独立的测试矩阵 capability，要求未来任何实现或重构都必须同时交付单元测试、集成测试、压力测试、疲劳测试与回归测试。
- 将现有实现中高风险的领域显式纳入规范，例如生命周期管理、跨线程唤醒、超时与取消语义、资源释放与测试环境可重复性。
- 将本 change 定位为长期维护的规范母版与迁移路线图；后续运行时实现应拆分到独立 change 中推进，而不是直接在本 change 内按阶段落代码。

## 能力域

### 新增 Capabilities
- `runtime-scheduler`: 定义 Scheduler 与 Processer 的职责边界、调度行为、偷取策略与停机协议。
- `coroutine-lifecycle`: 定义 Coroutine 与 Context 的生命周期、状态迁移、异常传播与资源回收要求。
- `event-hooking`: 定义 CoPoller、CoPollEvent 与系统调用 Hook 的事件注册、触发、取消和错误语义。
- `sync-primitives`: 定义 Chan、CoWaiter、CoCond、CoMutex、CoRWMutex 的等待/唤醒、关闭、超时和公平性要求。
- `coroutine-pool-and-api`: 定义 CoPool 与公共入口 API、语法糖、线程上下文约束和可观测行为。
- `runtime-test-matrix`: 定义所有模块都必须满足的测试层次、覆盖范围、环境约束与验收准入标准。

### 修改的 Capabilities

- 无。

## 影响范围

- 受影响代码：`bbt/coroutine/detail/*`、`bbt/coroutine/sync/*`、`bbt/coroutine/pool/*`、`bbt/coroutine/syntax/*`、`bbt/coroutine/coroutine.*`
- 受影响测试：`unit_test/*`、`debug/*`、`benchmark_test/*`
- 受影响流程：未来任何 runtime 相关变更在进入实现完成态之前，都需要与各模块 spec 以及新的测试矩阵保持一致；实现应优先通过独立的、按阶段拆分的 change 来推进
