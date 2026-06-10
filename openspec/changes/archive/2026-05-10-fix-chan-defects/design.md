## Context

Chan 是 bbtools-coroutine 的核心同步原语，提供有缓冲(`Chan<T, Max>`)和无缓冲(`Chan<T, 0>`)两种协程通道。当前实现使用 `m_is_reading` 原子标志实现读端 CAS 互斥，使用 `CoWaiter` 实现阻塞/唤醒，使用 `std::mutex` 保护队列。

`enrich-chan-tests` 变更通过 55 个测试用例暴露了 5 个缺陷，涉及 `__TChan.hpp` 中 TryRead/Write/Close 的多条执行路径。核心问题是：提前返回路径未正确清理资源（原子标志、互斥锁），以及 Close 唤醒后缺少状态重检。

## Goals / Non-Goals

**Goals:**
- 修复所有 5 个已确认缺陷，使已有的 55 个测试全部通过（包括之前标记为"已知缺陷"的 #23、#24）
- 确保所有方法的所有退出路径正确释放 `m_is_reading` 和 `m_item_queue_mutex`
- 确保 Close 唤醒的协程不执行违反 Close 语义的操作
- 明确 `Chan<T,0>` 的 API 边界

**Non-Goals:**
- 不重新设计 CAS 互斥模型（单读者模型是有意设计）
- 不修改 CoWaiter/CoPollEvent 的实现
- 不添加新的公开 API
- 不引入 RAII guard 包装（保持与现有代码风格一致）
- 不修改测试用例（测试作为验证基准）

## Decisions

### D1: 修复策略 — 逐路径补全 cleanup，不引入 RAII guard

**选择**: 在每条提前返回路径上显式添加 `m_is_reading = false` 和 `_UnLock()` 调用，不引入 scope guard 或 RAII wrapper。

**理由**: 项目中 Read/Write/ReadAll 已经使用显式的 `_Lock()`/`_UnLock()` 配对模式。引入 RAII guard 仅用于 2 个方法会造成风格不一致。受影响路径有限（约 6 处），逐一修复更直观。

**替代方案**: 引入 `ScopedLock` + `ScopedReading` guard — 更安全但与 CoWaiter 回调中释放锁的模式冲突（`_WaitUntilEnableRead` 的 callback 中调用 `_UnLock()`），RAII 析构时机与回调释放冲突。

### D2: Write 唤醒后 re-check — IsClosed + 容量双重检查

**选择**: `Write()` 被 `_WaitUntilEnableWrite` 唤醒后，在 `_Lock()` 之前先检查 `IsClosed()`，`_Lock()` 之后再检查队列容量。

**理由**: Close 会 Notify 所有阻塞的 write cond，多个 writer 同时被唤醒时可能都尝试 push。必须在获取锁后再次验证队列有空间。IsClosed 检查在 Lock 之前是因为 Close 后无需再获取锁（避免死锁）。

### D3: TryWrite(timeout) — 超时/失败时不 push

**选择**: `_WaitUntilEnableWriteOrTimeout` 返回非 0 时，直接返回对应错误码，不执行后续的 `m_item_queue.push(item)`。成功时（返回 0）同样需要 IsClosed + 容量双重检查。

**理由**: 当前代码无论等待结果如何都无条件 push，这是明显的逻辑错误。

### D4: Chan<T,0> API — 通过 public using 声明暴露 Try 系列

**选择**: 将 `Chan<T,0>` 从 `protected` 继承改为 `private` 继承，然后通过 `public: using BaseType::TryRead; using BaseType::TryWrite; using BaseType::ReadAll;` 显式暴露。

**理由**: protected 继承的意图是限制 API，但实际效果是使这些方法处于"可见但不可用"的尴尬状态。private 继承 + 显式 using 使 API 契约更清晰。无缓冲 Chan 在 buffer=1 的基础上运行这些方法，行为虽然与"纯无缓冲"直觉有差异，但已在 enrich-chan-tests 中记录了实际行为，保持可用优于隐藏。

**替代方案**: 保持 protected 继承 + 文档标注 — 选择不采用，因为"不可访问的 public 方法"对用户是困惑的。

### D5: Read/ReadAll 被唤醒后提前返回路径的修复

**选择**: `Read()` 中 `_WaitUntilEnableRead` 返回 -1 时，需要重置 `m_is_reading`（当前缺失）。`Read()` 被唤醒后 `_Lock()` 再检查 `m_item_queue.empty() || IsClosed()` 的 return -1 路径同样需要 `_UnLock()` + `m_is_reading = false`。`ReadAll()` 同理。

**理由**: 这些是 enrich-chan-tests 附带发现的锁泄露，与 #1/#2 同源。

## Risks / Trade-offs

- **[性能]** 唤醒后增加的 re-check（IsClosed + size check）引入极微量开销 → 仅在阻塞-唤醒路径上执行，非热路径，可忽略
- **[语义变更]** Close 后被唤醒的 Write 从返回 0 变为返回 -1 → 这是正确语义，之前的行为是 bug
- **[Chan<T,0> 行为]** TryRead/TryWrite 暴露后，用户可能对其行为有误解（实际基于 buffer=1 运作） → 文档和测试已覆盖实际行为
- **[并发正确性]** 多处修改涉及锁和原子操作的交互 → 已有 55 个测试覆盖，包括并发竞争场景
