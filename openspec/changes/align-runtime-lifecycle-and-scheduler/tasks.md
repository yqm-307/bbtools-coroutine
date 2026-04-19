## 1. 生命周期与调度边界建模

- [ ] 1.1 盘点 `Scheduler`、`Processer`、`Coroutine`、`Context` 当前共享状态，标明哪些字段跨线程参与调度、steal、stop 与 teardown。
- [ ] 1.2 明确 coroutine / runnable queue / event 对象的所有权模型，先冻结规则，再讨论具体封装类型。
- [ ] 1.3 定义阶段 2 的 stop 协议阶段：请求停止、唤醒 worker、处理残留 runnable、等待线程退出、进入终态。
- [ ] 1.4 确认阶段 2 严格消费阶段 1 的桥接稳定面，不重新定义等待完成模型。

## 2. Scheduler 与 Processer 收敛

- [ ] 2.1 收敛 `Scheduler` 的运行状态、负载均衡索引与 steal 索引，使其具有明确的同步语义。
- [ ] 2.2 收敛 `Processer` 的 runnable 消费、挂起唤醒和 stop 状态，使 worker 退出不依赖含糊的轮询与状态组合。
- [ ] 2.3 明确全局激活路径、本地执行路径和 work-steal 路径之间的边界，避免协程所有权重复转移。

## 3. Coroutine 与 Context 收敛

- [ ] 3.1 收敛 `Coroutine` 的创建、执行、挂起、重新激活、终态与 teardown 规则。
- [ ] 3.2 收敛 `Context::YieldWithCallback()` 与 `Coroutine::YieldWithCallback()` 的顺序保证，确保事件注册不会早于 suspended 状态成立。
- [ ] 3.3 收敛异常路径下的 await event 清理、状态迁移与错误上报顺序。
- [ ] 3.4 收敛 StackPool 相关的生命周期边界，确保 stack reuse / shrink 不破坏活跃协程正确性。

## 4. 文件级测试出口

- [ ] 4.1 基于现有 `Test_coroutine.cc`、`Test_coroutine_exception.cc`、`Test_coroutine_stack.cc` 整理阶段 2 的可复用测试资产与缺口。
- [ ] 4.2 新增 `Test_scheduler_shutdown.cc`，覆盖 idle stop、residual runnable stop 与 worker 唤醒顺序。
- [ ] 4.3 新增 `Test_scheduler_concurrency.cc`，覆盖 load-balance、work-steal、并发投递与 stop 边界。
- [ ] 4.4 新增 `Test_coroutine_lifecycle_state.cc`，覆盖合法状态迁移、awaitable suspend 和非法迁移可观测性。
- [ ] 4.5 新增 `Test_coroutine_lifecycle_teardown.cc`，覆盖 residual coroutine teardown、yield-with-callback 顺序与异常路径清理。
- [ ] 4.6 新增 `Test_coroutine_lifecycle_stress.cc`，覆盖 repeated start-stop、resume/yield 压力和异常与 teardown 交叠。

## 5. 验收与衔接

- [ ] 5.1 验证阶段 2 的实现产物可以作为 `migrate-sync-primitives-to-wait-protocol` 的稳定依赖层。
- [ ] 5.2 确认 scheduler 与 lifecycle 相关 requirement 场景都已映射到具体测试出口。
- [ ] 5.3 为阶段 3 `migrate-sync-primitives-to-wait-protocol` 准备后续 change，确保它只消费已冻结的 runtime 核心行为。