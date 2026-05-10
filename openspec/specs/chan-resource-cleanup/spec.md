## ADDED Requirements

### Requirement: TryRead 成功后必须重置 m_is_reading
系统 SHALL 在 `TryRead(item)` 成功读取元素后重置 `m_is_reading` 为 false，确保后续 Read 族操作可正常通过 CAS。

#### Scenario: TryRead 成功后 Read 可用
- **WHEN** 向有缓冲 Chan 写入 2 个元素，调用 TryRead 成功读出第 1 个元素后，再调用 Read
- **THEN** Read SHALL 通过 CAS 成功进入并读出第 2 个元素，返回 0

### Requirement: TryRead 队列为空失败时必须重置 m_is_reading 并释放锁
系统 SHALL 在 `TryRead(item)` 因队列为空返回 -1 时，重置 `m_is_reading` 为 false 并释放 `m_item_queue_mutex`。

#### Scenario: TryRead 队列空失败后 Read 可用
- **WHEN** 对空的有缓冲 Chan 调用 TryRead 返回 -1 后，另一协程写入元素，再调用 Read
- **THEN** Read SHALL 通过 CAS 成功进入，返回 0

### Requirement: TryRead(timeout) 超时后队列仍空时必须重置 m_is_reading
系统 SHALL 在 `TryRead(item, timeout)` 因超时唤醒且队列仍为空而返回 -1 时，重置 `m_is_reading` 为 false 并释放 `m_item_queue_mutex`。

#### Scenario: TryRead(timeout) 超时后 Read 可用
- **WHEN** 对空的有缓冲 Chan 调用 TryRead(item, 100) 超时返回 -1 后，另一协程写入元素，再调用 Read
- **THEN** Read SHALL 通过 CAS 成功进入，返回 0

### Requirement: TryRead(timeout) 成功路径必须重置 m_is_reading
系统 SHALL 在 `TryRead(item, timeout)` 成功读取元素后重置 `m_is_reading` 为 false。

#### Scenario: TryRead(timeout) 成功后 Read 可用
- **WHEN** 调用 TryRead(item, 500)，期间另一协程写入使其成功返回 0 后，再调用 Read
- **THEN** Read SHALL 通过 CAS 成功进入

### Requirement: Read 被唤醒后提前返回时必须重置 m_is_reading 并释放锁
系统 SHALL 在 `Read(item)` 中 `_WaitUntilEnableRead` 返回 -1（被 Close 唤醒）时重置 `m_is_reading` 为 false。系统 SHALL 在 `Read(item)` 被唤醒后重新 _Lock() 再检查发现队列为空或已关闭时，重置 `m_is_reading` 为 false 并释放锁。

#### Scenario: Read 被 Close 唤醒后资源正确释放
- **WHEN** 协程 A 因 Read 挂起，协程 B 调用 Close 唤醒协程 A，协程 A 的 Read 返回 -1
- **THEN** 后续对同一 Chan 的 Read 调用 SHALL 不会因 m_is_reading 仍为 true 而永久返回 -1

### Requirement: ReadAll 被唤醒后提前返回时必须重置 m_is_reading
系统 SHALL 在 `ReadAll(items)` 中 `_WaitUntilEnableRead` 返回 -1 时重置 `m_is_reading` 为 false。

#### Scenario: ReadAll 被 Close 唤醒后资源正确释放
- **WHEN** 协程 A 因 ReadAll 挂起（队列空），协程 B 调用 Close 唤醒协程 A，ReadAll 返回 -1
- **THEN** 后续对同一 Chan 的 Read 调用 SHALL 不会因 m_is_reading 仍为 true 而永久返回 -1
