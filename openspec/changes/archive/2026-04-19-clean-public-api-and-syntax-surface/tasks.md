## 1. Public API 收敛

- [x] 1.1 梳理 `coroutine.hpp` 当前暴露面的启动顺序、线程上下文和 failure mode。
- [x] 1.2 把这些前置条件和失败模式收敛成稳定契约，不再依赖隐含约定。

## 2. Syntax 收敛

- [x] 2.1 梳理 `bbtco`、`bbtco_sleep`、`bbtco_yield`、`bbtco_wait_for` 与事件辅助宏当前映射到底层的方式。
- [x] 2.2 确认 syntax 层只消费前四阶段稳定行为，而不再推动底层协议变化。

## 3. 文档与验证

- [x] 3.1 收敛 example 与 README，使其与稳定契约一致。
- [x] 3.2 新增 public API / syntax 验收测试，覆盖启动顺序、线程上下文和典型 failure mode。

## 4. 归档准备

- [x] 4.1 确认整条阶段链条的行为已经可以作为长期基线归档到 `openspec/specs/`。