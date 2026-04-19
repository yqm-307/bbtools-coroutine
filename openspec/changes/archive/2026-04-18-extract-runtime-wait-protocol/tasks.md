## 1. 协议建模

- [x] 1.1 定义共享等待协议层的核心抽象，包括等待节点、等待状态和完成结果。
- [x] 1.2 明确 timeout、notify、close、cancel、destroy 的竞争完成规则，并写入实现设计。
- [x] 1.3 明确哪些接口属于协议稳定面，哪些仍属于 runtime 内部实现细节。
- [x] 1.4 盘点当前所有直接拼装 `Coroutine::RegistCustom()`、`YieldWithCallback()`、`YieldUntilFd*()` 或 `GetLastResumeEvent()` 的等待入口，并标注其 consumer 类型。
- [x] 1.5 定义 protocol consumer 到桥接层的最小稳定面，禁止新接入点继续直接依赖上述原始接口。
- [x] 1.6 明确桥接面必须同时覆盖 custom wait、custom wait + timeout、fd wait + optional timeout 三类输入。

## 2. runtime 桥接

- [x] 2.1 收敛共享等待协议与 `Coroutine`、`CoPollEvent`、`CoPoller` 之间的桥接边界。
- [x] 2.2 设计 `CoWaiter` 的迁移方案，明确其在阶段 1 中先作为 façade 保留还是被新抽象直接替代。
- [x] 2.3 为带 timeout 的 custom wait 定义统一桥接方案，禁止 `CoMutex::TryLock(int ms)` 这类路径继续停留在“表面有 timeout、底层无统一 timeout 语义”的状态。
- [x] 2.4 验证共享等待协议可以同时描述 `sync` 的 custom wait、带 timeout 的 custom wait 与 Hook 形态的 fd/timeout wait，而不产生双重语义。
- [x] 2.5 确定阶段 1 的 Hook 验证方式，只要求存在最小可验证接入，不要求本阶段完成全部 Hook 迁移。

## 3. 渐进迁移

- [x] 3.1 先将 `CoWaiter` 收敛为兼容 façade 或受控适配层，避免新旧协议语义继续扩散。
- [x] 3.2 再迁移 `CoCond`，验证 waiter 生命周期和 timeout 后失效语义。
- [x] 3.3 再迁移 `CoMutex`，验证 owner、timeout 与唤醒策略。
- [x] 3.4 为 `Chan` 与 `CoRWMutex` 准备迁移入口和兼容约束，但不要求本 change 完成其最终重构。
- [x] 3.5 清理已经被试点替代的局部等待拼装逻辑，但允许受控兼容层在阶段 1 末尾暂时存在。
- [x] 3.6 明确 `CoPool` 与 public API 在阶段 1 仅作为下游消费者保持兼容，不在本 change 中直接收口其最终外部契约。

## 4. 全面测试

- [x] 4.1 以 `unit_test/Test_cond.cc`、`unit_test/Test_comutex.cc`、`unit_test/Test_hook.cc`、`unit_test/Test_coevent.cc` 与 `unit_test/Test_poller.cc` 为基线，整理阶段 1 的文件级测试资产清单，并显式标注不适合作为 CI 准入的用例。
- [x] 4.2 基于 `unit_test/Test_cond.cc` 扩展或拆出 `CoWaiter` / `CoCond` 的协议接入测试，覆盖 basic wait/notify、timeout 完成与多 waiter 唤醒。
- [x] 4.3 基于 `unit_test/Test_comutex.cc` 补一组真正验证 timeout-aware custom wait 的测试，证明 `CoMutex::TryLock(int ms)` 通过桥接面表达 timeout 结果，而不是只证明接口可运行。
- [x] 4.4 基于 `unit_test/Test_hook.cc` 的 `pipe` / 自建 listener 场景，以及 `unit_test/Test_coevent.cc` 的 fd/timeout 素材，补 hook-shaped wait 与 `bbtco_wait_for` 的桥接验证，并避免依赖宿主 `ssh` 服务的用例作为准入测试。
- [x] 4.5 基于 `unit_test/Test_poller.cc` 补共享等待协议层状态机与取消夹具，覆盖 pending、completed、timed-out、closed、canceled 等状态与完成结果。
- [x] 4.6 新增竞争完成回归测试，覆盖 notify-vs-timeout、close-vs-timeout、cancel-vs-notify 的单次完成保证。
- [x] 4.7 新增 waiter 有效性测试，覆盖 stale waiter 跳过、timeout 后再次扫描安全性以及 destroy/teardown flush。
- [x] 4.8 为 `unit_test/Test_chan.cc`、`unit_test/Test_co_rwmutex.cc` 与 `unit_test/Test_copool.cc` 标注阶段 1 仅保留的间接覆盖范围，并记录其协议级缺口留待后续迁移填补。
- [x] 4.9 新增共享等待协议层专项压力测试，验证高迭代下不出现重复完成、悬挂 waiter、卡死或泄漏。
- [x] 4.10 建立阶段 1 测试出口清单，把协议状态机、竞争完成、waiter 有效性、试点 sync 集成、hook-shaped 集成与压力测试六组结果绑定为进入阶段 2 runtime 核心重构的前置条件。

建议的新增测试文件基线：`Test_wait_protocol_state.cc`、`Test_wait_protocol_race.cc`、`Test_wait_protocol_waiter_lifetime.cc`、`Test_wait_protocol_bridge_sync.cc`、`Test_wait_protocol_bridge_fd.cc`、`Test_wait_protocol_stress.cc`。

## 5. 验收与衔接

- [x] 5.1 验证本 change 的实现产物可以作为后续 `sync`、`Hook` 和 runtime 核心重构的稳定依赖层。
- [x] 5.2 确认测试矩阵覆盖了阶段 1 的所有 requirement 场景。
- [x] 5.3 为阶段 2 runtime 核心重构准备后续 change，确保下一阶段只围绕共享协议稳定面推进。