## ADDED Requirements

### Requirement: Write 被 Close 唤醒后必须返回失败
系统 SHALL 在 `Write(item)` 中协程因缓冲区满挂起、被 Close 唤醒恢复后，检测到 Chan 已关闭时返回 -1，不执行 push 操作。

#### Scenario: 单个 Write 被 Close 唤醒
- **WHEN** 协程 A 因 Write 挂起（缓冲区满），协程 B 调用 Close
- **THEN** 协程 A 的 Write SHALL 返回 -1，不向队列写入元素

#### Scenario: 多个 Write 被 Close 同时唤醒
- **WHEN** 3 个协程因 Write 挂起（缓冲区满），另一协程调用 Close
- **THEN** 3 个协程的 Write SHALL 均返回 -1，不向队列写入元素，不产生崩溃

### Requirement: Write 被唤醒后必须验证队列容量
系统 SHALL 在 `Write(item)` 被正常唤醒（非 Close 场景，由 Read 消费后触发）后，重新检查队列容量。若队列仍满（被其他 writer 抢先写入），SHALL 重新挂起或返回失败。

#### Scenario: 多个 Write 被正常唤醒竞争
- **WHEN** 2 个协程因 Write 挂起（容量为 1 的 Chan 已满），读端消费 1 个元素唤醒 1 个 writer
- **THEN** 被唤醒的 writer SHALL 成功 push，另一个 SHALL 继续等待或失败，队列大小 SHALL 不超过 Max

### Requirement: TryWrite(timeout) 超时后不得 push
系统 SHALL 在 `TryWrite(item, timeout)` 因超时返回时，不执行 `m_item_queue.push(item)`。

#### Scenario: TryWrite(timeout) 超时不写入
- **WHEN** 容量为 1 的 Chan 已满，调用 TryWrite(item, 100) 等待 100ms 超时
- **THEN** TryWrite SHALL 返回非 0 值，队列大小 SHALL 仍为 1（未新增元素）

### Requirement: TryWrite(timeout) 被 Close 唤醒后不得 push
系统 SHALL 在 `TryWrite(item, timeout)` 被 Close 唤醒后，检测到 Chan 已关闭时返回 -1，不执行 push。

#### Scenario: TryWrite(timeout) 被 Close 唤醒
- **WHEN** 协程 A 因 TryWrite(item, 5000) 挂起（缓冲区满），协程 B 调用 Close
- **THEN** 协程 A 的 TryWrite SHALL 返回 -1，不向队列写入元素

### Requirement: TryWrite(timeout) 成功唤醒后验证容量
系统 SHALL 在 `TryWrite(item, timeout)` 被正常唤醒（有空间）后，重新检查 IsClosed 和队列容量，确认仍可写入后才执行 push。

#### Scenario: TryWrite(timeout) 唤醒后仍可写
- **WHEN** 容量为 1 的 Chan 已满，TryWrite(item, 500) 挂起，50ms 后读端消费
- **THEN** TryWrite SHALL 返回 0，元素成功写入
