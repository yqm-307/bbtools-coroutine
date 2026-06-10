## ADDED Requirements

### Requirement: 运算符失败路径返回 false
系统 SHALL 在 << 运算符写失败和 >> 运算符读失败时返回 false。

#### Scenario: Close 后 << 返回 false
- **WHEN** Chan Close 后调用 `chan << value`
- **THEN** SHALL 返回 false

#### Scenario: Close 后 >> 返回 false
- **WHEN** Chan Close 后调用 `chan >> value`
- **THEN** SHALL 返回 false

### Requirement: shared_ptr 版本运算符
系统 SHALL 支持 `std::shared_ptr<Chan>` 的 << 和 >> 运算符，行为与值版本一致。

#### Scenario: shared_ptr << 写入
- **WHEN** 通过 `chan_ptr << value` 写入
- **THEN** SHALL 返回 true，随后 Read 可读出该值

#### Scenario: shared_ptr >> 读取
- **WHEN** 先通过 `chan_ptr << value` 写入，再通过 `chan_ptr >> val` 读取
- **THEN** SHALL 返回 true，val 等于写入值

### Requirement: 有缓冲 Chan FIFO 顺序保证
系统 SHALL 保证有缓冲 Chan 的读取顺序严格按写入顺序（先入先出）。

#### Scenario: 单写单读 FIFO 顺序
- **WHEN** 向容量为 100 的 Chan 按顺序写入 0, 1, 2, ..., 99
- **THEN** Read SHALL 按 0, 1, 2, ..., 99 的顺序读出

### Requirement: 无缓冲 Chan 写入值与读取值匹配
系统 SHALL 保证无缓冲 Chan 读取值与写入值一致。

#### Scenario: 无缓冲多次读写值匹配
- **WHEN** 向 `Chan<int, 0>` 依次写入 10, 20, 30（每次写后读）
- **THEN** 三次 Read SHALL 分别得到 10, 20, 30

### Requirement: 多写者并发数据完整性
系统 SHALL 在多写者并发写入时保证数据不丢失、不重复。

#### Scenario: 多写者并发写入后逐一消费
- **WHEN** 10 个写者协程各写入 0-9，1 个读者消费全部 100 个值
- **THEN** 读取的值集合 SHALL 为 10 个 {0,1,...,9} 的并集，总数恰好 100，无丢失
