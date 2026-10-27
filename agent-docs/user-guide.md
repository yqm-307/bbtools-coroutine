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

## P 本地调度与升级注意事项

每个 P 使用一个 CFS 就绪集合：新提交、global 补入、yield 和事件唤醒均参与相同排序；同等虚拟服务量按入队先后选择。记账随 Coroutine 保存，跨 P 偷取或退役经 global 中转时按两端本地基准重定位，不清零历史债务。唤醒不积攒无限等待信用。

`m_cfg_processer_priority_runtime_budget_us` 沿用名称和默认 `{50, 150, 200, 600}`，含义已改为 LOW/NORMAL/HIGH/CRITICAL 的相对服务权重，按 `max(1, elapsed_us) / max(1, weight)` 推进虚拟服务量；旧配置中的零预算不再禁用某优先级。这是配置语义变化，依赖旧预算含义的调用方需要调整；配置仍须在 Start 前完成。

CFS 仍是协作式调度：单次 Resume 不被强制抢占，业务长期不让出时不能承诺等待上界。P 队列内部使用短 mutex，锁不跨 Resume、park/yield 发布或外部 driver Wake；全局队列与通知协议保持既有边界。Coroutine/Processer 内部布局发生变化，升级后须重编本库及直接依赖，不混用旧头文件和二进制。

Agent 加载 `.github/skills/bbtools-coroutine/SKILL.md`。改运行时实现仍读 `AGENTS.md` 与 `agent-docs/2026-09-07-core-runtime-contract.md`。涉及网络 I/O 集成先读契约 §11。
