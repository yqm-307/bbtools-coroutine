## Why

Chan 的单元测试覆盖严重不足：当前仅有 9 个测试用例，仅覆盖了 Write/Read 的基本阻塞路径和运算符重载。TryWrite/TryRead 系列（4个方法）、ReadAll、Close 生命周期、并发竞争、数据完整性几乎是空白。与此同时，代码审查发现了 `TryRead()` 和 `TryRead(timeout)` 中 `m_is_reading` 未正确重置的潜在缺陷——这正是测试缺失的直接后果。

## What Changes

- 新增约 45 个测试用例，覆盖 Chan 的全部公开 API 和关键边界行为
- 覆盖 7 大方向：基础功能正确性、TryWrite 系列、TryRead 系列、Close 生命周期、并发竞争、运算符、数据完整性
- 有缓冲 Chan (`Chan<T, Max>`, Max > 0) 和无缓冲 Chan (`Chan<T, 0>`) 分别验证
- 通过测试暴露并确认 `m_is_reading` 泄露缺陷

## Capabilities

### New Capabilities
- `chan-basic-correctness`: 有缓冲/无缓冲 Chan 的基本读写值正确性、不同元素类型、ReadAll 功能
- `chan-try-operations`: TryWrite/TryRead 的成功/失败/超时路径，含 m_is_reading 泄露缺陷验证
- `chan-close-lifecycle`: Close 后各操作返回值、重复 Close 安全性、Close 唤醒阻塞协程、析构自动 Close
- `chan-concurrency`: 多读竞争、CAS 互斥验证、Read 完成后恢复可用性、无缓冲多读者
- `chan-operator-and-integrity`: 运算符失败路径、shared_ptr 版本运算符、FIFO 顺序、并发完整性

### Modified Capabilities

## Impact

- 仅影响 `unit_test/Test_chan.cc`，不修改任何实现代码
- 新增测试依赖已有基础设施：`bbt::core::thread::CountDownLatch`、`bbtco` 宏、`sync::CoWaiter`
- 部分测试（#23、#24）可能暴露 `__TChan.hpp` 中的 `m_is_reading` 泄露缺陷，需后续修复
