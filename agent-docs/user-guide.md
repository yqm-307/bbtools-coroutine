# bbtools-coroutine 使用手册

调用本库的说明按主题组织，与仓库 skill 相同。

| 模块 | 路径 |
|------|------|
| 架构简介 | `.github/skills/bbtools-coroutine/references/architecture.md` |
| 基本用法 / 进阶用法 / 使用范式 | `.github/skills/bbtools-coroutine/references/usage.md` |
| API 参考（仅当前事实） | `agent-docs/api-reference.md` |
| 显式协程声明与调用（`bbt_co_async(T)` / `bbt_co_await`） | [API 参考 §2](api-reference.md#2-协程注册与调用语法) |
| 坑点与禁止行为 | `.github/skills/bbtools-coroutine/references/pitfalls.md` |
| 执行域与网络 I/O 域边界（目标架构） | `agent-docs/2026-09-07-core-runtime-contract.md` §11 |

Agent 加载 `.github/skills/bbtools-coroutine/SKILL.md`。改运行时实现仍读 `AGENTS.md` 与 `agent-docs/2026-09-07-core-runtime-contract.md`。涉及网络 I/O 集成先读契约 §11。
