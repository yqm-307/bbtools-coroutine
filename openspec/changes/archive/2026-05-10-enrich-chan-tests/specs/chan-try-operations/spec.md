## ADDED Requirements

### Requirement: 有缓冲 TryWrite 立即成功/失败
系统 SHALL 在 TryWrite 调用时，若缓冲区未满立即写入成功返回 0，若缓冲区已满立即返回 -1 不挂起协程。

#### Scenario: 缓冲区未满 TryWrite 成功
- **WHEN** 创建容量为 10 的 Chan 并调用 TryWrite
- **THEN** SHALL 返回 0，随后 Read 可读出写入的值

#### Scenario: 缓冲区已满 TryWrite 失败
- **WHEN** 创建容量为 1 的 Chan，Write 填满后调用 TryWrite
- **THEN** SHALL 返回 -1，协程不挂起

### Requirement: 有缓冲 TryWrite 超时路径
系统 SHALL 在 TryWrite(item, timeout) 调用时，若缓冲区满则挂起直到超时或有空间。

#### Scenario: 缓冲区满等待超时
- **WHEN** 容量为 1 的 Chan 已满，调用 TryWrite(item, 100)
- **THEN** SHALL 在约 100ms 后返回 -1

#### Scenario: 缓冲区满期间被消费后成功
- **WHEN** 容量为 1 的 Chan 已满，调用 TryWrite(item, 500) 的同时另一协程在 100ms 后 Read
- **THEN** TryWrite SHALL 返回 0

### Requirement: 有缓冲 TryRead 立即成功/失败
系统 SHALL 在 TryRead 调用时，若队列非空立即读出返回 0，若队列为空立即返回 -1 不挂起协程。

#### Scenario: 队列非空 TryRead 成功
- **WHEN** 向 Chan Write 一个元素后调用 TryRead
- **THEN** SHALL 返回 0，读出值与写入一致

#### Scenario: 队列为空 TryRead 失败
- **WHEN** Chan 队列为空时调用 TryRead
- **THEN** SHALL 返回 -1，协程不挂起

### Requirement: 有缓冲 TryRead 超时路径
系统 SHALL 在 TryRead(item, timeout) 调用时，若队列为空则挂起直到超时或有数据。

#### Scenario: 队列空等待超时
- **WHEN** Chan 队列为空，调用 TryRead(item, 100)
- **THEN** SHALL 在约 100ms 后返回 -1

#### Scenario: 队列空期间写入后成功
- **WHEN** Chan 队列为空，调用 TryRead(item, 500) 的同时另一协程在 100ms 后 Write
- **THEN** TryRead SHALL 返回 0，读出值与写入一致

### Requirement: TryRead 成功后 m_is_reading 必须重置
系统 SHALL 在 TryRead 成功读取后重置 m_is_reading 为 false，使后续 Read 族操作可正常进入。

#### Scenario: TryRead 成功后 Read 可用
- **WHEN** 调用 TryRead 成功读出一个元素后，再次调用 Read
- **THEN** Read SHALL 能正常进入（CAS 成功），不返回 -1

### Requirement: TryRead 超时后队列仍空时 m_is_reading 必须重置
系统 SHALL 在 TryRead(item, timeout) 因超时且队列仍为空而提前返回时重置 m_is_reading 为 false。

#### Scenario: TryRead 超时后 Read 可用
- **WHEN** TryRead(item, 100) 因超时且队列为空返回 -1 后，另一协程写入元素，再调用 Read
- **THEN** Read SHALL 能正常进入（CAS 成功），不返回 -1

### Requirement: 无缓冲 Chan TryWrite/TryRead 行为
系统 SHALL 记录 `Chan<T,0>` 继承的 TryWrite/TryRead 的实际行为。

#### Scenario: 无缓冲 TryWrite
- **WHEN** 创建 `Chan<int, 0>` 并调用 TryWrite
- **THEN** SHALL 记录返回值（记录实际行为）

#### Scenario: 无缓冲 TryRead
- **WHEN** 创建 `Chan<int, 0>` 并调用 TryRead
- **THEN** SHALL 记录返回值（记录实际行为）
