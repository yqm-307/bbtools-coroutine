## 背景

阶段 5 的任务不是再改底层，而是把前四阶段已经冻结的行为重新投影到用户真正接触的入口上：`coroutine.hpp`、语法糖宏、事件辅助器、example 与 README。

## 当前代码证据

- `coroutine.hpp` 当前几乎是能力总汇，但只在少量注释中隐含表达启动顺序和线程上下文要求。
- `bbtco_yield`、`bbtco_sleep`、`bbtco_wait_for` 等宏直接触底层 API，若没有明确契约，用户仍会把内部实现偶然性当成公共行为。
- 事件辅助宏和 `CoPool` 辅助路径当前更像功能入口，而不是被文档化的稳定契约。

## 目标 / 非目标

**目标：**

- 明确 public API 的启动顺序、线程上下文与失败模式。
- 让语法糖严格反映稳定 runtime 行为，而不是掩盖关键前置条件。
- 收敛 example 与 README，使其与稳定契约一致。

**非目标：**

- 不再修改前四阶段的底层协议、lifecycle、sync、Hook 或 `CoPool` 基础语义。
- 不引入新的底层能力域。

## 设计决策

### 1. 语法糖只能消费稳定行为，不能再次扩张行为面

阶段 5 之后，如果某个宏行为仍需要推动底层改动，应视为前一阶段边界没有收住，而不是继续在 syntax 层兜底。

### 2. example 与 README 应成为契约验证材料，而不是单纯演示

示例和文档应覆盖启动顺序、线程上下文、Hook 使用方式和常见 failure mode，而不是只展示 happy path。

## 建议测试文件拆分

- `unit_test/Test_public_api_contract.cc`
- `unit_test/Test_syntax_macro_contract.cc`

## 迁移计划

1. 先梳理 public API 与 syntax 当前隐含的前置条件。
2. 再把这些前置条件、失败模式和成功路径显式文档化。
3. 最后用 public API / syntax 验收测试和 example / README 一起收口。