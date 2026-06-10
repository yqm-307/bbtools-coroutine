## ADDED Requirements

### Requirement: 多协程并发 Read 的 CAS 互斥
系统 SHALL 保证同一时间仅有一个协程成功进入 Read（通过 m_is_reading 的 CAS），其他协程的 Read 调用返回 -1。

#### Scenario: 两个协程同时 Read 第二个失败
- **WHEN** 两个协程同时尝试对同一 Chan 调用 Read（队列为空，会阻塞）
- **THEN** 仅一个协程的 Read SHALL 能进入（CAS 成功），另一个 SHALL 返回 -1

#### Scenario: 两个协程同时 TryRead CAS 竞争
- **WHEN** 两个协程同时尝试 TryRead（队列非空）
- **THEN** 仅一个协程 SHALL 成功返回 0，另一个 SHALL 因 CAS 失败返回 -1

### Requirement: Read 完成后后续 Read 可正常进入
系统 SHALL 在一次 Read 完成并重置 m_is_reading 后，允许新的 Read 调用成功进入。

#### Scenario: Read 完成后再 Read
- **WHEN** 协程 A 成功 Read 一个元素，随后协程 B 调用 Read
- **THEN** 协程 B 的 Read SHALL 能成功进入（CAS 成功）

### Requirement: 多读者并发读有缓冲 Chan 数据完整性
系统 SHALL 在多读者并发读有缓冲 Chan 时保证数据不丢失、不重复。

#### Scenario: 2 个读者并发读
- **WHEN** 向容量为 100 的 Chan 写入 100 个递增整数，2 个读者并发 Read 直到全部消费
- **THEN** 两个读者读取的值集合 SHALL 为 {0, 1, ..., 99}，无重复无丢失

### Requirement: 无缓冲 Chan 多读者场景
系统 SHALL 记录 `Chan<T,0>` 在多读者场景下的行为（因 CAS 互斥，同时仅一个读者可进入）。

#### Scenario: 无缓冲多读者
- **WHEN** `Chan<int, 0>` 有 2 个读者协程并发 Read
- **THEN** SHALL 记录实际行为（CAS 互斥导致仅一个读者能成功进入）
