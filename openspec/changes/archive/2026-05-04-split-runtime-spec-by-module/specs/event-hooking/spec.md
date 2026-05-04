## ADDED Requirements

### Requirement: Event objects define registration, trigger, cancel, and finalization semantics
runtime SHALL 为 CoPollEvent 对象定义完整生命周期，包括初始化、注册、触发、取消与终结。每个事件实例 MUST 最多只进入一次终态。

#### Scenario: Event is triggered by fd readiness
- **WHEN** 已注册的 fd 事件变为 readable 或 writable
- **THEN** 该事件只经历一次触发处理并完成终结，不发生重复回调投递

#### Scenario: Event is canceled before trigger
- **WHEN** 某个事件在就绪或超时发生前被取消注册
- **THEN** runtime 取消该监听操作，且该事件之后不会再投递成功唤醒回调

### Requirement: Custom wakeup and timeout semantics are unambiguous
runtime SHALL 明确定义自定义唤醒与带超时等待之间的交互语义。当存在多个可能的唤醒源时，runtime MUST 对外暴露究竟是哪一种来源恢复了协程。

#### Scenario: Custom wakeup arrives before timeout
- **WHEN** 协程在带超时的自定义事件上等待，且自定义事件先被触发
- **THEN** 协程以自定义事件语义恢复，而不会被报告为 timeout 情况

#### Scenario: Timeout wins the race
- **WHEN** 协程在带 timeout 的 fd 或自定义事件上等待，且 timeout 先到达
- **THEN** runtime 以 timeout 语义恢复该协程，并清理底层注册状态

### Requirement: Hooked blocking operations preserve documented I/O semantics
runtime SHALL 定义在协程执行期间，`socket`、`connect`、`accept`、`read`、`write`、`send`、`recv` 与 `sleep` 等被 Hook 的阻塞操作行为。被 Hook 的操作 MUST 要么通过非阻塞重试与事件等待正确完成，要么通过文档化的错误路径失败。

#### Scenario: Hooked connect completes asynchronously
- **WHEN** 某次非阻塞 `connect` 无法立即完成并需要等待 writable 就绪
- **THEN** runtime 等待正确的就绪条件，并在报告成功前验证最终 `connect` 结果

#### Scenario: Hooked read or write encounters retryable errors
- **WHEN** 被 Hook 的 `read`、`write`、`send`、`recv` 或 `accept` 返回 `EAGAIN`、`EINTR` 等可重试错误
- **THEN** runtime 按照文档化策略挂起并重试，而不是阻塞 worker 线程

### Requirement: Hook usage is constrained by thread context
runtime SHALL 定义 Hook 行为何时生效，包括 thread-local 协程上下文如何启用或禁用拦截。发生在已启用协程线程之外的调用 MUST 回退到文档化的非 Hook 行为。

#### Scenario: Blocking API is called outside coroutine-enabled thread context
- **WHEN** 在禁用协程支持的线程中调用被 Hook 的 API
- **THEN** runtime 使用底层 system call 路径，而不是协程挂起行为

#### Scenario: Blocking API is called inside a coroutine-enabled worker
- **WHEN** 在已启用的 worker 线程上的协程中调用被 Hook 的 API
- **THEN** runtime 使用事件驱动的挂起行为，而不是阻塞 OS 线程
