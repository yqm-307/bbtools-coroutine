# bbtools-coroutine skill 维护契约

本文件是该 skill 的维护契约（Charter），维护流程入口是 `skill-charter`。

## 定位与非目标

- 定位：指导**调用** bbtools-coroutine 库（`bbtco` / Scheduler / Chan / CoMutex / CoPool / Hook）的应用代码。
- 非目标：不描述运行时实现（实现契约真源是 `AGENTS.md` 与 `agent-docs/2026-09-07-core-runtime-contract.md`）；不承载框架 Service/路由/重试语义；不规定宿主安装、加载与投影机制。

## 来源与版本

- 来源：本仓自研，无第三方上游；迁移前不存在 `UPSTREAM.md`。
- 版本：随本仓分支演进，无独立版本号。

## 允许与禁止扩张

- 允许：与源码/测试核对后的事实修正（签名、返回码、生命周期、宏名、示例路径）；按需新增 `references/`。
- 禁止：改动 frontmatter 的触发语义（`name`/`description`）、权限/流程/门禁；复制 `agent-docs/api-reference.md` 的签名正文；新增只存在于宏里的状态机。

## 外部依赖与权威来源

- 权威来源优先级：头文件 + 测试 > `agent-docs/api-reference.md` > README 速查表。
- 可编译示例对照：`example/`。

## 兼容与失效条件

- 公共面变更（新增/删除 API、生命周期契约变化）后，相关表述立即视为待核对；失效判定以头文件与 `agent-docs/2026-09-07-core-runtime-contract.md` 为准。

## 验证入口与回退边界

- 事实修正的最低验证：核对源码/测试 + 静态检查；不强制模型 A/B。
- 无独立 lint 入口；回退靠 git diff（改动最小、可恢复）。
