## Why

`enrich-chan-tests` 测试变更暴露了 Chan 实现中 5 个缺陷：`m_is_reading` 原子标志在多条 TryRead 路径上泄露导致后续所有 Read 族操作永久失败；Close 唤醒多个阻塞 Write 协程时无条件 push 导致队列溢出崩溃；TryWrite(timeout) 超时/唤醒后同样无条件 push；以及多处锁未释放的资源泄露。这些缺陷在并发场景下可触发内存违规和死锁，必须修复。

## What Changes

- 修复 `TryRead()` 和 `TryRead(timeout)` 所有退出路径的 `m_is_reading` 重置和锁释放
- 修复 `Write()` 被 Close 唤醒后无条件 push 的问题，增加唤醒后 re-check
- 修复 `TryWrite(timeout)` 超时/失败后无条件 push 的问题
- 修复 `Read()` 和 `ReadAll()` 中被唤醒后提前返回路径的锁和 m_is_reading 泄露
- 重新设计 `Chan<T,0>` 的继承方式，将 TryRead/TryWrite/ReadAll 通过 using 声明暴露或明确标记为不支持

## Capabilities

### New Capabilities
- `chan-resource-cleanup`: 确保 Chan 所有 Read/Write 族方法在所有退出路径上正确释放 m_is_reading 和 mutex
- `chan-close-write-safety`: 确保 Close 唤醒阻塞 Write 协程后不会产生队列溢出或语义违规
- `chan-unbuffered-api`: 明确 Chan<T,0> 的 API 可见性设计

### Modified Capabilities

## Impact

- 修改文件：`bbt/coroutine/sync/__TChan.hpp`、`bbt/coroutine/sync/Chan.hpp`
- 影响所有使用 Chan 的协程通信代码
- 不改变 API 签名，仅修复内部逻辑（#5 除外，可能调整 Chan<T,0> 的可见性）
- 现有通过的测试应继续通过；之前标记为 "已知缺陷" 的测试 (#23, #24) 应在修复后通过
