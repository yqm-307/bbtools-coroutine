## ADDED Requirements

### Requirement: Waiter-based synchronization objects have safe waiter lifetime management
runtime SHALL 保证 CoWaiter、CoCond、Chan、CoMutex 与 CoRWMutex 使用的 waiter 对象在 wait、wakeup、timeout、取消和 teardown 路径上都保持有效。timeout 或取消 MUST NOT 留下之后仍可能被触发的陈旧 waiter 引用。

#### Scenario: Waiter times out before notification
- **WHEN** 某个协程以带 timeout 的方式等待，且 timeout 在任何 notifier 运行前先到达
- **THEN** 同步对象会移除或中和该 waiter，从而使后续 `Notify` 不会访问无效 waiter 状态

#### Scenario: Synchronization object is destroyed with pending waiters
- **WHEN** 同步对象在仍有 waiter 挂起时被 close 或销毁
- **THEN** 所有待处理 waiter 都会通过文档化的关闭路径被恢复或失败返回，而不是被永久阻塞

### Requirement: Chan defines close, timeout, and single-reader semantics explicitly
runtime SHALL 明确定义带缓冲与无缓冲 Chan 在 `Read`、`Write`、`TryRead`、`TryWrite`、`ReadAll`、`Close` 及其 timeout 变体下的行为。失败或 timeout 的操作 MUST 恢复内部状态，使后续操作仍然有效。

#### Scenario: Read path fails after entering waiting mode
- **WHEN** 某个 `Read` 或带 timeout 的 `Read` 已进入等待状态，随后因 timeout、close 或唤醒失败而返回失败
- **THEN** Chan 会恢复未来 reader 与 writer 安全继续运行所需的内部 reading 标志或锁状态

#### Scenario: Channel is closed with blocked readers and writers
- **WHEN** 在 reader 或 writer 正在等待时调用 `Close`
- **THEN** 所有阻塞操作都会按照文档化的 close 语义被恢复或终止，且不会留下悬挂 waiter

#### Scenario: Unbuffered channel pairs reader and writer
- **WHEN** 无缓冲 Chan 中已有待配对 writer，且随后 reader 变得可用
- **THEN** 恰好发生一次数据交接，且内部 rendezvous 状态在完成后保持一致

### Requirement: CoCond defines notification semantics under timeout and spurious ordering races
runtime SHALL 定义 CoCond 的 `Wait`、`WaitFor`、`NotifyOne` 与 `NotifyAll` 行为，使 timeout 与通知竞争在处理上既安全又具确定性。

#### Scenario: WaitFor times out before NotifyOne
- **WHEN** 某个在 `WaitFor` 中等待的协程因 timeout 恢复，且先于其他线程或协程调用 `NotifyOne`
- **THEN** 后续 `NotifyOne` 不会访问无效 waiter 内存，并且在存在下一个有效 waiter 时能继续处理它

#### Scenario: NotifyAll runs with mixed valid and expired waiters
- **WHEN** 调用 `NotifyAll` 时，部分 waiter 已经超时或因其他原因被取消
- **THEN** 该操作会安全跳过失效 waiter，并精确地恢复每个剩余有效 waiter 一次

### Requirement: CoMutex and CoRWMutex define ownership, timeout, and fairness semantics
runtime SHALL 为 CoMutex 与 CoRWMutex 定义加锁、timeout 行为、unlock 所有权以及 waiter 唤醒策略。基于 timeout 的 API MUST 严格遵守请求的 timeout，而不能静默退化为无 timeout 等待。

#### Scenario: CoMutex TryLock with timeout expires
- **WHEN** 调用带 timeout 的 `TryLock`，且锁在 timeout 到期前无法获取
- **THEN** 该调用按 timeout 语义返回，且不会留下陈旧 waiter 注册

#### Scenario: CoRWMutex prevents writer starvation by policy
- **WHEN** reader 与 writer 长时间持续竞争
- **THEN** runtime 遵循文档化的唤醒策略，在保持正确性的同时使饥饿行为显式可测
