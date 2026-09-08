# M1-05d 崩溃与栈溢出诊断边界：评估结论与锁行为

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md` §5
Issue：#278

## 评估结论（锁定，非新造）

- **栈溢出诊断边界 = 页保护 + SIGSEGV fail-fast**：
  - `Stack::_ApplyStackProtect` 在栈底 `mprotect(PROT_NONE)` 一个 guard page
    （`m_cfg_stack_protect=true`，默认开）。越界读写直接 SIGSEGV 落在现场，
    不是静默跑飞/踩他人内存。
  - **锁行为**：新增 `Test_crash_diag`——16KB 栈 + 深递归（200 层 × 2KB）
    在 fork 子进程中必然 SIGSEGV（实测 WTERMSIG==SIGSEGV）。
  - `m_cfg_stack_protect=false` 时无 guard page，溢出行为未定义——
    该开关是性能/安全的显式取舍（README 已说明），不新增检测。
- **断言路径**：`Assert`/`AssertWithInfo` 由 `bbt/core/util/Assert.hpp` 决定
  （Release=NDEBUG 打 stderr 继续，Debug=abort）。行为随构建类型翻转，
  不在本测试锁死（避免脆弱断言）。运行时内部使用 `Assert` 的 12 处均为
  不变式保护，非错误处理通道。
- **fcontext 异常边界**：协程内用户异常由 Context 入口 `catch(...)` 隔离并
  走 #267/#275 交付链（eptr 保存/日志+计数），异常**不会**穿越栈保护页边界，
  栈溢出（SIGSEGV）与异常（C++）是两条独立诊断路径。

## 明确不做（超 M1 边界）

- 信号处理器/backtrace/sigaltstack 崩溃现场转储：属独立工程（第三方依赖、
  符号化、异步安全），不在 M1 范围。契约已保证 SIGSEGV 落在现场 + 栈页
  边界明确，后续 Issue 再评估最小方案。
- 协程栈水线（high-water mark）采样：收益不抵开销，暂不做。

## 验证

- 新增 `Test_crash_diag`：fork-before-Start 子进程溢出 SIGSEGV（父进程 waitpid
  收尸判定，不污染测试进程）。
- 全量 `ctest -j2` 24/24。
