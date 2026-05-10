## Context

当前 Chan 的测试仅覆盖 Write/Read 的基本阻塞路径和运算符重载，共 9 个测试用例。Chan 公开 API 包含 9 个方法（Write、Read、TryWrite×2、TryRead×2、ReadAll、Close、IsClosed）和 4 个运算符重载，但测试仅触及其中约 1/3 的行为路径。

代码审查发现 `__TChan.hpp` 中 `TryRead()` 成功路径和 `TryRead(timeout)` 超时后队列为空的提前返回路径均未重置 `m_is_reading`，导致后续所有 Read 族操作因 CAS 失败而永久返回 -1。这是测试缺失的直接后果。

所有新增测试仅修改 `unit_test/Test_chan.cc`，使用现有 Boost.Test 框架和项目内同步原语（CountDownLatch、CoWaiter），不引入新依赖。

## Goals / Non-Goals

**Goals:**
- 将 Chan 测试覆盖率从约 30% API 路径提升至接近 100%
- 系统性覆盖 TryWrite/TryRead 系列的所有成功/失败/超时路径
- 覆盖 ReadAll 的完整行为（阻塞读空、一次性读出、与 Write 交互）
- 覆盖 Close 生命周期的完整语义（关闭后操作、重复关闭、唤醒阻塞协程、析构关闭）
- 覆盖并发竞争场景（多读 CAS 竞争、Read 后恢复、无缓冲多读者）
- 覆盖运算符失败路径和 shared_ptr 版本
- 覆盖数据完整性（FIFO 顺序、不丢不重）
- 暴露并确认 m_is_reading 泄露缺陷

**Non-Goals:**
- 不修改任何实现代码（缺陷确认后另行修复）
- 不重构现有测试用例
- 不添加性能/压力测试（已有 benchmark_test/fatigue_chan.cc）
- 不覆盖内部保护方法（_Lock、_OnEnableRead 等）

## Decisions

### D1: 测试组织方式 — 全部追加在 Test_chan.cc

**选择**: 在现有 Test_chan.cc 中追加新测试用例，不拆分文件。

**理由**: 项目中每个同步原语对应一个测试文件（Test_chan.cc、Test_comutex.cc、Test_cond.cc），保持一致性。现有文件仅 299 行，追加后仍可控。

**替代方案**: 按功能拆分为多个文件（Test_chan_basic.cc、Test_chan_close.cc 等）— 增加构建复杂度，且与项目惯例不符。

### D2: 超时精度 — 使用 100ms 作为基础超时

**选择**: TryRead/TryWrite 的超时测试统一使用 100ms 作为超时值，配合 `bbt::core::clock::gettime()` 做时间断言。

**理由**: 100ms 在协程调度环境下足够稳定（不会因调度抖动误判），同时测试执行时间不会过长。更短的值（10ms）容易因调度延迟导致 flaky test。

### D3: 并发竞争测试使用 CountDownLatch 同步

**选择**: 使用 `bbt::core::thread::CountDownLatch` 确保协程启动并到达竞争点后再触发关键操作。

**理由**: 与现有测试风格一致（t_chan_1_vs_1、t_chan_1_vs_n 均使用 CountDownLatch）。CoWaiter 也可用但 CountDownLatch 更直观。

### D4: m_is_reading 泄露测试标记为已知缺陷

**选择**: 为 #23（TryRead 成功后 m_is_reading 未重置）和 #24（TryRead timeout 超时后队列空提前返回）编写测试，但预期它们会失败。使用 `BOOST_CHECK` 而非 `BOOST_ASSERT`，失败时记录但不 crash。

**理由**: 这些测试的目的是确认缺陷存在，而非验证正确行为。缺陷修复后这些测试应自然通过。

### D5: 无缓冲 Chan 继承方法的行为验证

**选择**: 对 `Chan<T,0>` 继承的 TryWrite/TryRead/ReadAll 方法编写测试，记录实际行为（无论是否符合直觉），作为未来重构的基准。

**理由**: `Chan<T,0>` 继承自 `Chan<T,1>` 但仅 override 了 Write/Read/Close，TryWrite/TryRead/ReadAll 的行为由父类决定。在缓冲区大小为 1 的约束下，这些方法的行为可能与用户对"无缓冲"的预期不同。记录实际行为比猜测更有价值。

## Risks / Trade-offs

- **[Flaky Tests]** 协程调度不确定性可能导致超时测试和竞争测试偶发失败 → 使用 100ms 超时、CountDownLatch 同步、不依赖精确时序断言
- **[m_is_reading 缺陷测试失败]** #23、#24 预期失败，但 Boost.Test 会报告失败数增加 → 明确标注为缺陷验证测试，修复后自然通过；或临时使用 BOOST_WARN 而非 BOOST_CHECK
- **[无缓冲继承行为意外]** Chan<T,0> 的 TryRead/TryWrite/ReadAll 行为可能不符合"无缓冲"直觉 → 仅记录行为，不评判对错
- **[测试运行时间]** 新增约 45 个测试，含阻塞/超时测试 → 大部分超时使用 100ms，整体运行时间预计增加 5-10s
