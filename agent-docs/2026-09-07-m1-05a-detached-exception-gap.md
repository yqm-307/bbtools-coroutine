# M1-05a Detached 异常交付与未处理异常策略：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md` §5
Issue：#275（依赖 #267，PR 链基于 feat/issue-267-exception-stop）

## 已锁住

- 异常终态：detached 协程用户代码异常在 fcontext 入口统一 `catch (...)`，
  `OnException(eptr)` 保存 `std::exception_ptr` + 进入 `CO_FINAL`（#267）。
- 交付优先级：有回调 → `m_ext_coevent_exception_callback`（what 文本，回调
  自身异常隔离，不逃出 fcontext）；无回调 → **日志 + 计数**（本次补日志）。
- 日志格式：`[bbtco] unhandled exception co=<id>: <what截断200字节>` 到 stderr，
  带协程 id 便于与现场快照（#276）串联。
- 计数：`m_unhandled_exception_count` 原子自增，测试锁可观测。
- Timeout 保持返回值语义，不转异常（#267）。

## 关键实现约束（踩坑记录）

- 该日志**不能用 `bbt::core::log::WarnPrint`**：catch 运行在协程自身栈上，
  DebugPrint 族内部 `vformat` + `VPrint` 各带 `char[4096]` 栈缓冲（合计 ≈8KB），
  会打爆默认 4KB 协程栈（实测 memory access violation）。改用直接 `fprintf`
  到 stderr，栈占用有界。任何在协程异常/挂起路径上打日志的代码同理受限。
- `%.200s` 截断防长 what 撑爆 stderr 行缓冲。

## 验证

- `Test_exception_stop` 7/7：`t_unhandled_exception_counted_without_callback`
  运行输出中可见日志行 `[bbtco] unhandled exception co=2: count-panic`；
  计数断言 + 日志由本用例执行输出共同锁定。
- 全量 `ctest -j2` 22/22（本分支基点）。
