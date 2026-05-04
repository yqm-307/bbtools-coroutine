## ADDED Requirements

### Requirement: Every runtime module has a multi-layer test plan
任何涉及 runtime scheduling、协程生命周期、事件/Hook 行为、同步原语或协程池行为的实现变更 SHALL 包含一份跨越单元测试、集成测试、回归测试以及压力或疲劳测试的测试计划。

#### Scenario: Change touches a single runtime module
- **WHEN** 某个 change 仅修改单一 runtime 模块
- **THEN** 该实现除局部行为的单元测试外，还包含至少一个集成测试或回归测试，用于证明该模块仍能与相邻模块正确协作

#### Scenario: Change crosses module boundaries
- **WHEN** 某个 change 修改了跨多个 runtime 模块的行为
- **THEN** 该实现除为每个直接受影响模块提供单元覆盖外，还包含跨模块集成测试

### Requirement: Test scenarios cover normal, error, timeout, teardown, and contention paths
runtime 测试套件 SHALL 不仅覆盖成功路径，也覆盖每个受影响模块的错误处理、timeout 竞争、teardown/shutdown 行为以及并发竞争路径。

#### Scenario: Synchronization or event logic gains a new timeout path
- **WHEN** 某个 change 新增或修改了带 timeout 感知的行为
- **THEN** 配套测试必须包含“成功先于 timeout”“timeout 先于成功”以及“timeout 期间发生 teardown”三类场景

#### Scenario: Scheduler or pool logic changes stop behavior
- **WHEN** 某个 change 影响了 shutdown、release 或 teardown 时序
- **THEN** 测试套件按适用情况覆盖 idle 状态下 stop、存在待处理任务时 stop，以及重复 start-stop 或 release 周期

### Requirement: Tests are environment-reproducible and CI-suitable
runtime 测试套件 SHALL 避免隐式机器假设，例如依赖固定端口上的宿主服务、无限制 `sleep`，或在未由测试夹具显式提供时依赖外部 daemon。

#### Scenario: Hook or networking behavior is tested
- **WHEN** 某个测试验证 `socket`、`connect`、`accept`、`send`、`recv` 或 `wait-for` 行为
- **THEN** 该测试必须自行提供确定性的对端或受控夹具，而不是假设系统服务已存在

#### Scenario: Timing-sensitive behavior is tested
- **WHEN** 某个测试依赖 timeout 或调度节奏
- **THEN** 它必须使用有界等待、显式夹具或可重复的时序控制，从而保证结果在 CI 和负载条件下仍保持稳定

### Requirement: Known bugs and high-risk regressions become permanent regression tests
当 waiter 生命周期、所有权、stop 语义、Hook 行为或队列状态恢复中识别出缺陷时，runtime SHALL 在修复被视为完成前加入一个可复现该类缺陷的回归测试。

#### Scenario: A stale waiter or lifecycle bug is fixed
- **WHEN** 维护者修复了涉及 timeout waiter、协程所有权泄漏或陈旧队列状态的 bug
- **THEN** 必须新增一个在旧行为下失败、在修正后行为下通过的回归测试

### Requirement: Stress and fatigue tests protect concurrency-sensitive guarantees
对于正确性依赖并发、重复唤醒或长时间运行的模块，runtime SHALL 维护足够规模的压力或疲劳测试覆盖，以验证这些保证。

#### Scenario: Change affects scheduler, Chan, mutexes, or CoPool
- **WHEN** 某个 change 修改了并发敏感的 runtime 路径
- **THEN** 验证计划必须包含高迭代次数或多线程压力覆盖，以暴露重复唤醒、waiter 卡死、所有权竞争或 shutdown 泄漏等问题
