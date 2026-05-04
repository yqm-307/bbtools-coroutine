## MODIFIED Requirements

### Requirement: Waiter-based synchronization objects have safe waiter lifetime management
runtime SHALL 保证 `CoWaiter`、`CoCond`、`Chan`、`CoMutex` 与 `CoRWMutex` 使用的 waiter 对象在 wait、wakeup、timeout、close 和 teardown 路径上都保持可控生命周期。timeout 或 close 之后 MUST NOT 留下后续仍可能被触发的陈旧 waiter 引用。

#### Scenario: Timeout leaves no stale waiter behind
- **WHEN** 某个同步原语上的 waiter 已因 timeout 返回，而后续通知路径再次扫描到它
- **THEN** 该同步原语会安全跳过或中和该 waiter，而不会重复恢复或访问失效状态

#### Scenario: Destroy or close flushes pending waiters
- **WHEN** `CoCond` 或 `Chan` 在仍有 waiter 挂起时进入销毁或关闭路径
- **THEN** 所有待处理 waiter 都会通过文档化的终止路径被恢复或失败返回，而不会永久阻塞

### Requirement: Chan defines close, timeout, and single-reader semantics explicitly
runtime SHALL 明确定义带缓冲与无缓冲 `Chan` 在 `Read`、`Write`、`TryRead`、`TryWrite`、`ReadAll`、`Close` 及其 timeout 变体下的行为。失败或 timeout 的操作 MUST 恢复内部状态，使后续操作仍然有效。

#### Scenario: Timed read restores reader state
- **WHEN** 某个带 timeout 的 `Read` 或 `TryRead` 因 timeout 返回失败
- **THEN** Chan 会恢复内部 reader 状态，使后续 `Write` 与下一次 `Read` 仍可继续成功推进

#### Scenario: Close interrupts blocked channel operations
- **WHEN** reader 或 writer 正在等待时调用 `Close`
- **THEN** 所有阻塞操作都会按照文档化的 close 语义被恢复或终止，且不会留下悬挂 waiter

#### Scenario: Unbuffered rendezvous completes at most once
- **WHEN** 无缓冲 `Chan` 中已有待配对 writer，随后 reader 变得可用或 close 发生竞争
- **THEN** 数据交接与失败返回二者至多发生一次，且内部 rendezvous 状态保持一致

### Requirement: CoCond defines notification semantics under timeout races
runtime SHALL 定义 `CoCond` 的 `Wait`、`WaitFor`、`NotifyOne` 与 `NotifyAll` 行为，使 timeout 与通知竞争在处理上既安全又具确定性。

#### Scenario: NotifyOne skips an expired waiter
- **WHEN** 某个 `WaitFor` 已因 timeout 恢复，随后 `NotifyOne` 扫描到该 waiter
- **THEN** 后续通知路径不会访问无效 waiter，并且在存在下一个有效 waiter 时能继续处理它

#### Scenario: NotifyAll wakes each valid waiter exactly once
- **WHEN** 调用 `NotifyAll` 时，等待队列中同时存在已过期 waiter 和仍然有效的 waiter
- **THEN** `NotifyAll` 会安全跳过失效 waiter，并精确恢复每个剩余有效 waiter 一次

### Requirement: CoMutex and CoRWMutex define ownership, timeout, and fairness semantics
runtime SHALL 为 `CoMutex` 与 `CoRWMutex` 定义加锁、unlock 所有权、timeout 行为以及 waiter 唤醒策略。任何基于 timeout 的等待 MUST 严格遵守请求的 timeout；`CoRWMutex` 的公平性策略 MUST 可观测且可测试。

#### Scenario: CoMutex timeout does not leak waiter state
- **WHEN** 调用带 timeout 的 `TryLock`，且锁在 timeout 到期前无法获取
- **THEN** 该调用按 timeout 语义返回，且不会留下阻塞后续 waiter 的陈旧注册

#### Scenario: CoRWMutex keeps waiting writer ahead of late readers
- **WHEN** 已有 writer 进入等待队列，而新的 reader 在 writer 仍未获取锁时到达
- **THEN** runtime 按文档化公平性策略阻止 late reader 越过已等待 writer

#### Scenario: Non-owner unlock does not release write ownership
- **WHEN** 非持有者协程尝试对处于写锁状态的 `CoRWMutex` 调用 `UnLock`
- **THEN** 该操作不会释放当前写锁，也不会让其他 waiter 误以为锁已正常移交
