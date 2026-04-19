## 背景

前四个阶段稳定的是内部协议、runtime 核心、sync 中层以及 Hook / `CoPool` 中上层行为。最后一个阶段需要把这些稳定语义收束到公共入口和语法糖层，让 `coroutine.hpp`、`SyntaxMacro.hpp`、example 和文档真正反映库的稳定行为，而不是历史实现细节。

当前代码里已经有两个明显信号表明这一步需要独立成 change：

- `coroutine.hpp` 当前把大量能力直接暴露给用户，但启动顺序、线程上下文和失败模式主要靠隐含约定而不是清晰契约。
- `SyntaxMacro.hpp` 里的 `bbtco`、`bbtco_sleep`、`bbtco_yield`、`bbtco_wait_for`、事件辅助宏直接映射到底层能力，如果前四阶段不把这些行为重新投射出来，用户仍然会透过语法糖碰到底层实现偶然性。

## 变更内容

- 引入阶段 5 的独立 change，收敛公共 API、语法糖、example 与文档出口。
- 让 `coroutine-pool-and-api` spec 中的启动顺序、线程上下文和诊断要求在 public surface 上具备稳定落点。
- 确保语法糖不掩盖 failure mode，也不再反向驱动底层协议变化。

## 能力域

### 修改的 Capabilities
- `coroutine-pool-and-api`

### 新增 Capabilities

- 无。

## 影响范围

- 受影响代码：`bbt/coroutine/coroutine.hpp`、`bbt/coroutine/syntax/*`、`example/*`、`README.md`
- 受影响测试：阶段 4 之前产出的稳定行为测试，以及后续新增的 public API / syntax 验收测试
- 受影响流程：本阶段完成后，整条重构链条才具备面向用户的稳定出口