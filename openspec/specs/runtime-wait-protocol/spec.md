## ADDED Requirements

### Requirement: Shared wait protocol defines a single wait lifecycle
runtime SHALL 为一次等待定义统一生命周期，覆盖创建、注册、等待中、被唤醒、超时、被关闭打断、被取消以及最终完成。任何依赖等待语义的模块 MUST 通过该统一生命周期与 runtime 交互，而不是局部定义独立协议。

#### Scenario: Wait enters pending state
- **WHEN** 某个模块发起一次新的等待并完成注册
- **THEN** 该等待进入统一定义的待完成状态，并具有唯一身份与可追踪状态

#### Scenario: Wait finishes exactly once
- **WHEN** timeout、notify、close 或 cancel 中的多个完成源发生竞争
- **THEN** 只有一个完成源可以成功结束该等待，其余完成源只能观察到该等待已结束

### Requirement: Wait completion exposes a normalized result model
runtime SHALL 为每次等待暴露统一的完成结果模型，使调用方能够区分成功完成、超时完成、关闭打断、取消打断以及内部失败，而不依赖局部副作用组合推断语义。

#### Scenario: Timeout completes a wait
- **WHEN** 某次等待因 timeout 达到完成条件
- **THEN** 调用方收到明确的 timeout 完成结果，而不是通过事件位和局部标志自行推断

#### Scenario: Close interrupts a wait
- **WHEN** 某次等待因对象关闭或销毁被打断
- **THEN** 调用方收到明确的关闭或终止结果，并能据此执行一致的清理路径

### Requirement: Wait protocol owns waiter validity
runtime SHALL 使等待协议层拥有 waiter 的有效性边界。waiter 在超时、取消、关闭或正常完成后 MUST 进入不可再次触发的状态，后续通知路径 MUST NOT 访问无效 waiter。

#### Scenario: Stale waiter is skipped after timeout
- **WHEN** 某个 waiter 已经因 timeout 结束，且随后有通知路径扫描到它
- **THEN** 通知路径会安全跳过该 waiter，而不会访问其失效状态或造成重复恢复

#### Scenario: Destroy path flushes pending waiters
- **WHEN** 某个拥有等待队列的对象进入关闭或销毁路径
- **THEN** 所有待处理 waiter 都会被统一终结，并从可通知集合中移除

### Requirement: Shared wait protocol is reusable across sync and hook paths
runtime SHALL 使共享等待协议能够同时支撑 `sync` 原语与阻塞 I/O 适配路径，而无需两侧分别维护不同的完成语义或生命周期模型。

#### Scenario: Sync primitive uses shared wait protocol
- **WHEN** `CoCond`、`Chan`、`CoMutex` 或 `CoRWMutex` 发起等待
- **THEN** 它们通过共享等待协议表达等待与完成，而不是直接依赖局部事件生命周期拼装

#### Scenario: Hook path uses shared wait protocol
- **WHEN** 某个阻塞 I/O Hook 需要等待 fd 就绪或 timeout
- **THEN** 它能够通过共享等待协议接入 runtime，而不需要重复定义独立等待完成语义

### Requirement: Shared wait protocol exposes a stable bridge surface over raw coroutine primitives
runtime SHALL 在协议消费者与 `Coroutine`、`CoPollEvent`、`CoPoller` 的原始接口之间定义稳定桥接面。协议消费者 MUST NOT 继续直接依赖 `RegistCustom()`、`YieldWithCallback()`、`YieldUntilFd*()` 或 `GetLastResumeEvent()` 来拼装等待语义。

#### Scenario: Sync consumer arms a custom wait through the bridge
- **WHEN** 某个同步原语需要注册 custom wait 并在稍后由 notify 完成
- **THEN** 它通过共享等待协议的桥接面完成注册与结果获取，而不是直接操作 `Coroutine` 与 `CoPollEvent` 细节

#### Scenario: Timeout-aware custom wait uses the same bridge surface
- **WHEN** 某个同步原语需要注册带 timeout 的 custom wait，例如带超时的锁等待或条件等待
- **THEN** 它通过同一桥接面表达 timeout 条件与完成结果，而不是退化为无 timeout 的 custom wait 再由调用方自行推导结果

#### Scenario: Hook-shaped wait arms an fd wait through the bridge
- **WHEN** 某个 fd-ready 或 timeout 等待需要接入协议层
- **THEN** 它通过同一桥接面表达等待条件与完成结果，而不是直接读取 coroutine 的底层事件状态

### Requirement: Shared wait protocol is covered by dedicated tests
runtime SHALL 为共享等待协议维护专项测试，覆盖生命周期正确性、竞争完成、关闭打断、回归场景与高迭代压力场景。

#### Scenario: Regression test protects single-completion guarantee
- **WHEN** 修复了一类 wait 可能被重复完成的缺陷
- **THEN** 必须新增回归测试来证明同类竞争以后只会完成一次

#### Scenario: Stress test exercises repeated wait competition
- **WHEN** 协议层进入实现验收阶段
- **THEN** 测试计划必须包含高迭代或并发压力场景，用于暴露重复唤醒、陈旧 waiter 与关闭竞争问题