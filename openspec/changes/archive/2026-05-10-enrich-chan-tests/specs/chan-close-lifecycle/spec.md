## ADDED Requirements

### Requirement: Close 后所有操作必须返回失败
系统 SHALL 在 Chan Close 后，Write、Read、TryWrite、TryRead、ReadAll 均返回 -1，IsClosed 返回 true。

#### Scenario: Close 后 Write 返回 -1
- **WHEN** Chan Close 后调用 Write
- **THEN** SHALL 返回 -1

#### Scenario: Close 后 Read 返回 -1
- **WHEN** Chan Close 后调用 Read
- **THEN** SHALL 返回 -1

#### Scenario: Close 后 TryWrite 返回 -1
- **WHEN** Chan Close 后调用 TryWrite
- **THEN** SHALL 返回 -1

#### Scenario: Close 后 TryRead 返回 -1
- **WHEN** Chan Close 后调用 TryRead
- **THEN** SHALL 返回 -1

#### Scenario: Close 后 ReadAll 返回 -1
- **WHEN** Chan Close 后调用 ReadAll
- **THEN** SHALL 返回 -1

#### Scenario: Close 后 IsClosed 返回 true
- **WHEN** Chan Close 后调用 IsClosed
- **THEN** SHALL 返回 true

### Requirement: 重复 Close 必须安全
系统 SHALL 允许对已关闭的 Chan 重复调用 Close 而不崩溃或产生副作用。

#### Scenario: 重复 Close 安全
- **WHEN** 对同一 Chan 连续调用 Close 两次
- **THEN** SHALL 不崩溃，第二次 Close 正常返回

### Requirement: Close 唤醒阻塞中的 Read 协程
系统 SHALL 在 Close 时唤醒因 Read 而挂起的协程，Read 返回 -1。

#### Scenario: Close 唤醒阻塞的 Read
- **WHEN** 协程 A 因 Read 挂起（队列为空），协程 B 调用 Close
- **THEN** 协程 A 的 Read SHALL 返回 -1 并恢复执行

### Requirement: Close 唤醒阻塞中的 Write 协程
系统 SHALL 在 Close 时唤醒因 Write 而挂起的协程，Write 返回 -1。

#### Scenario: Close 唤醒单个阻塞的 Write
- **WHEN** 协程 A 因 Write 挂起（缓冲区满），协程 B 调用 Close
- **THEN** 协程 A 的 Write SHALL 返回 -1 并恢复执行

#### Scenario: Close 唤醒多个阻塞的 Write
- **WHEN** 3 个协程因 Write 挂起（缓冲区满），另一协程调用 Close
- **THEN** 3 个协程的 Write SHALL 均返回 -1 并恢复执行

### Requirement: Close 唤醒阻塞中的 TryRead(timeout) 协程
系统 SHALL 在 Close 时唤醒因 TryRead(timeout) 而挂起的协程。

#### Scenario: Close 唤醒阻塞的 TryRead(timeout)
- **WHEN** 协程 A 因 TryRead(item, 5000) 挂起，协程 B 调用 Close
- **THEN** 协程 A 的 TryRead SHALL 返回 -1 并恢复执行

### Requirement: Close 唤醒阻塞中的 TryWrite(timeout) 协程
系统 SHALL 在 Close 时唤醒因 TryWrite(timeout) 而挂起的协程。

#### Scenario: Close 唤醒阻塞的 TryWrite(timeout)
- **WHEN** 协程 A 因 TryWrite(item, 5000) 挂起（缓冲区满），协程 B 调用 Close
- **THEN** 协程 A 的 TryWrite SHALL 返回 -1 并恢复执行

### Requirement: 无缓冲 Chan Close 后操作返回失败
系统 SHALL 记录 `Chan<T,0>` Close 后各操作的返回值。

#### Scenario: 无缓冲 Close 后操作
- **WHEN** `Chan<int, 0>` Close 后调用 Write/Read
- **THEN** SHALL 返回 -1

### Requirement: 无缓冲 Chan Close 唤醒阻塞写协程
系统 SHALL 在无缓冲 Chan Close 时唤醒因 Write 而挂起的协程。

#### Scenario: 无缓冲 Close 唤醒阻塞写协程
- **WHEN** 协程 A 因无缓冲 Chan Write 挂起（无读端），另一协程调用 Close
- **THEN** 协程 A 的 Write SHALL 返回 -1 并恢复执行

### Requirement: Chan 析构自动 Close
系统 SHALL 在 Chan 析构时自动调用 Close。

#### Scenario: 析构触发 Close
- **WHEN** Chan 对象离开作用域被析构
- **THEN** SHALL 自动调用 Close，IsClosed 返回 true
