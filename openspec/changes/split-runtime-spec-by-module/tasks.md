## 1. 模块 Spec 基线

- [ ] 1.1 复核当前 runtime 源码布局，确认每个 capability 都能映射到稳定的职责边界。
- [ ] 1.2 为 `runtime-scheduler`、`coroutine-lifecycle`、`event-hooking`、`sync-primitives`、`coroutine-pool-and-api` 和 `runtime-test-matrix` 建立长期维护归属。
- [ ] 1.3 验证未来 runtime 变更能够引用一个主 capability，并仅在必要时附加跨模块依赖。

## 2. 阶段 1：冻结共享等待稳定面

- [ ] 2.1 完成 `extract-runtime-wait-protocol` change，冻结共享等待协议、完成结果与桥接稳定面。
- [ ] 2.2 确认阶段 1 已覆盖第一批直接 consumer：`CoWaiter`、`CoMutex`、Hook 路径与 `bbtco_wait_for` 语法糖路径。
- [ ] 2.3 确认阶段 1 已定义独立测试文件与测试用例基线，至少覆盖状态机、竞争完成、waiter 生命周期、sync 桥接、fd 桥接与压力测试六类验证。

## 3. 阶段 2：Scheduler 与生命周期对齐

- [ ] 3.1 对照 `runtime-scheduler` spec 收敛 Scheduler 与 Processer 的实现，重点处理共享状态同步与停机语义。
- [ ] 3.2 对照 `coroutine-lifecycle` spec 收敛 Coroutine、Context、StackPool 与 teardown 路径，重点处理所有权与终态回收。
- [ ] 3.3 增加或更新诊断能力，使非法状态迁移和停机异常在调试与测试阶段可观测。
- [ ] 3.4 确认阶段 2 只消费阶段 1 已冻结的桥接稳定面，不回头修改等待完成模型、基础状态机或阶段 1 测试出口定义。

## 4. 阶段 3：同步原语迁移

- [ ] 4.1 对照 `sync-primitives` spec 先迁移 `CoCond` 与 `CoMutex`，验证 waiter 生命周期、timeout-aware custom wait 与唤醒策略。
- [ ] 4.2 在前两者验证完成后再迁移 `Chan` 与 `CoRWMutex`，收敛 close、状态恢复与公平性相关语义。
- [ ] 4.3 确认阶段 3 只建立在前两个阶段已冻结的等待完成语义、协程生命周期边界与事件桥接顺序之上。

## 5. 阶段 4：Hook、CoPool 与上层行为收敛

- [ ] 5.1 对照 `event-hooking` spec 收敛 Hook 的 connect/read/write/send/recv/accept/sleep 行为，覆盖触发、取消、超时与错误语义。
- [ ] 5.2 对照 `coroutine-pool-and-api` spec 收敛 `CoPool` 的提交、唤醒、阻塞等待与 release 语义，修复两个 `Submit` 重载的可见行为漂移。
- [ ] 5.3 确认阶段 4 不再修改 custom wait、fd wait 或 timeout 的基础完成语义；若仍需修改，应回退判定为阶段 1 未完成。

## 6. 阶段 5：公共 API 与测试矩阵收口

- [ ] 6.1 收敛 `coroutine.hpp`、`syntax/*`、example 与文档出口，确保它们只消费前述稳定行为而不反向推动底层协议变化。
- [ ] 6.2 建立按 capability 维度组织的测试清单，确保每个 requirement 场景至少映射到一个具体测试用例，并明确区分单元、集成、回归与压力测试策略。
- [ ] 6.3 为 scheduler stealing、Chan 竞争、mutex 公平性、CoPool 吞吐以及重复 start-stop/release 周期补充压力或疲劳测试覆盖，并用确定性测试夹具替代固定外部服务。
- [ ] 6.4 验证每个已实现的 runtime 变更都引用了对应模块 spec，并满足 `runtime-test-matrix` 的要求。
- [ ] 6.5 为当前实现与新 specs 存在实质偏差的模块准备后续 change。