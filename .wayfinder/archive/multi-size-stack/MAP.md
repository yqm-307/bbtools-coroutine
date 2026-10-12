---
label: wayfinder:map
status: archived
map_type: delivery
title_zh: 协程多尺寸栈支持
description_zh: 保留全局默认栈大小，支持协程显式选择大小，并按系统页对齐及正确复用。
route_status: confirmed
route_revision: 4
route_waypoints: [multi-size-stack-runtime]
---
# 协程多尺寸栈支持

## 仓库归档快照说明
本目录为已归档地图的可公开快照，`archive=incomplete`：代码已合并，诊断验收仍有缺口。不是新的活跃施工地图，不自动恢复或触发任务。
本地完整归档保留；此快照省略会话/临时运行记录，并把私有路径改为角色化占位路径。规格、设计、任务编号/勾选和未完成项不变，验收依据使用可核验的 GitHub 链接。

## Destination
在同一协程运行时中支持不同大小的固定栈共存：未指定大小使用 `g_bbt_coroutine_config->m_cfg_stack_size`，显式指定则使用请求大小并向上按系统页对齐；保留地址页对齐和栈保护，栈池按实际尺寸正确申请、复用与归还。

成功判据：
- 原有不带大小参数的调用行为不变，显式请求不修改全局默认配置。
- 不同大小的协程可同时运行、挂起和恢复；实际可用栈不小于请求，保护页不占用请求的可用栈大小。
- 同尺寸栈可以复用，不误取较小或保护属性不匹配的栈；归还按栈自身属性处理，不重新读取全局大小。
- 跨尺寸仍遵守统一容量约束，回收只触及空闲栈；非法输入、对齐及追加保护页的溢出、分配失败有明确且安全的行为。
- 用真实本地聚焦测试覆盖默认、显式、多尺寸共存、对齐和复用，并验证直接耦合调用；按仓库规则完成适用审查。缺少证据不得宣称实现完成。

## Route
用户已一次确认整图路线，仅一个完整路径点：**多尺寸固定栈端到端支持**（稳定 ID `multi-size-stack-runtime`）。canonical 四文件位于 `waypoint/multi-size-stack-runtime/`；规划细节和实际包级状态以四件套为准，ready 不授权实施。

本点覆盖：保留默认 C++/宏调用，增加显式大小 C++ 入口，接通 `Coroutine → Context → StackPool → Stack`；统一字节类型和合法值/页对齐检查；按实际可用大小及保护属性分桶；跨桶统一借出/预留容量、空闲回收与失败清理；同步旧小栈调用和相关测试，完成真实本地冒烟、功能与直接耦合验证及适用审查；全量 CTest 按当前本地仓规交 CI，后续 CI/远端动作需另行授权并核清实施基线的流程差异。

这是一个可独立验收的完整出口，不把调查、编码、测试分别切成路径点。生产/诊断构建与快速性能回归沿目标仓既有入口核验，不承诺未测性能。探索已取得路线确认并创建 draft 骨架；随后用户明确 `/wfer-continue plan` 并要求继续，本轮仅补齐四件套和计划审查，不进入实现；后续 apply 另按实际请求和权限推进。

## Constraints
- 已确认：省略大小参数走全局配置，显式指定走指定大小；地址按系统页对齐，可用大小向上按系统页取整。
- 已确认：同一运行时允许不同大小的 Stack 共存，不是运行中调整同一个已创建栈。
- 继续采用现有有栈协程与 C++17，不增加调度器实例或线程，不改变当前运行时生命周期。
- 本图已获准建图/探索与plan，随后用户明确 `/wfer-continue apply`，本轮授权单点本地实现/验证/修复和独立实现审查，含已列unit_test/CMakeLists.txt精确测试入口改动。实施只在专属worktree，owner原有AGENTS.md与旧HEAD保持，不fetch/pull；不改root构建入口/CI，不安装系统依赖，不提交/远端发布/CI/合并。新授权来源是当前用户apply指令，不是路线字段或ready状态。

## Notes
### 合并后交付及留缺口归档（当前事实，优先于下方历史）
用户已批准同runner base/head性能CI简化修复，并明确授权合并；PR #385 已合并到 main `b36f5c4cd4406162c093f4e886ee5cab3915e9c8`。PR最终head `b60860a22c559a9eed5cb12241fb089f25832509` 与merge树一致；main push CI `38225087248`、Docs Check `38225086317` 均success。main CTest首轮57/57、smoke14/reliability23 cases无错误；六模块各45s、2线程base/head真实比较全部PASS，未改正式基线/required/仓库变量。独立实现审查链及7文件CI增量审查GO已完成。

稳定证据：
- https://github.com/yqm-307/bbtools-coroutine/pull/385
- https://github.com/yqm-307/bbtools-coroutine/actions/runs/38225087248
- https://github.com/yqm-307/bbtools-coroutine/actions/runs/38225086317
- https://github.com/yqm-307/bbtools-coroutine/pull/385#issuecomment-6120962390

用户在看到缺口后明确「没事留缺口归档吧」。本次归档是incomplete，不撤销S13/S15诊断要求，不把6-2或完整7-3标为通过；不重开开发/压测、不安装工具、不发布版本。残留：STRINGENT_DEBUG基线shared_from_this编译失败，Valgrind工具/头缺未测；PR阶段Test_hook_fd_epoch首轮FD快照15→14、重试后通过，main首轮通过不代表根因已修；历史Poller首发abort未定位。无自有子代理/构建或CI在途，用户原AGENTS修改保留。以后需要补诊断时必须明确恢复本图并核对应候选与环境。

- 所属项目：`bbtools-coroutine`；地图身份由 `.wayfinder/project.json` 与本图 slug 确定。
- route_revision=4 同步当前用户apply的动作边界（旧3仅改Route真源引用）；路线成员、Destination、覆盖范围与验收不变。实施工作区和授权细目见proposal，不将技术索引当授权凭据。
- 已核事实：当前仍用 `memalign + mprotect`，不是 mmap；断点在 `Context` 丢掉大小及保护参数、`StackPool` 单池。保留现有分配方式及回调释放后归还栈的顺序。
- 研究结论区分事实与推荐，完整源码依据、容量口径、预热/回收兼容、输入/错误边界和真实验证范围均见[参数链路与栈池兼容性核查](tickets/parameter-pool-compatibility.md#resolution)。
- 技术候选：按页对齐后的实际可用大小及保护属性分桶，不强制二次幂档；最大同时借出/预留数、平均值与最低保留数仍全池统一，不给每桶复制上限/最低保留数。接口、同步/错误与验收机制现见 revision=2 四件套。独立计划审查 MSR-PR-01～07 已闭合；第二份报告曾因父会话修正 tasks 5-3 的序号错分类而哈希漂移、停止审批，并非最终通过。最终闭合报告已对重新冻结的实际候选确认哈希稳定、该纠正与 S10/S13 一致，无必修项；本轮仅置计划 ready，不表示运行时实现完成。
- 版本边界：本地 HEAD `ddfa93c8edad9ce1e6c68b3006dce32df9130d1c`；已获取 `origin/main` 为 `b172f53e66ee8c7e5008272d3a4eaccc23206647`，不是实时远端 HEAD 声明。实施前核实基线，不沿用旧 Stop/restart。未 fetch、pull 或切分支。
- 兼容风险：旧 `Coroutine::Create(4096, ...)` 及 debug 的 1024 参数以前被忽略；接通后是真正小栈，须同步核对调用和任务需求。大小类型统一可能影响 detail API 二进制 ABI，不能宣称旧二进制无需重编。
- plan阶段只有源码/结构及独立计划审查证据，不表示实现通过。当前apply已完成基线聚焦构建测试及显式大小真实RED，专属worktree已有未完成初稿并交单写入实现者；最终功能/回归/诊断/性能/CI尚需真实核验。未提交或写远端；计划审查仍不能替代实现审查。
- 规划闭合证据：首轮审查、问题增量复审和最终候选闭合报告均已由主会话实际读取；最终报告为 `wfer-multi-size-stack-plan-review-closure.md`（scratch 附件）。其开始/结束四文件哈希与主会话最终回读一致；tasks 5-3 的修改是纠正错误分类，而非等价措辞。以下记录最终被审内容的身份，避免依赖会被清理的临时日志。

| 四件套 | 被审 revision=2 的 SHA256 |
|---|---|
| `proposal.md` | `3d8d067ddeadd32a2ab3b3987bd0e8aeb501e1d22840ef44feb3e11a851f3776` |
| `specs.md` | `8dae5996f6d6a1f134ceedcdc3ea5ad35d3bebc899a6ca443cc00e511627d811` |
| `design.md` | `13732a83ee70907c07b45a8b9349d47dcecba4c87e8efb0150d2b97fbeab5557` |
| `tasks.md` | `c71d346583ada23d28517dd90789bf5c975ab5e5ec55e66470300f1e4c8073ab` |

- `proposal.md` 在审查后仅由 `status: draft` 改为 `status: ready`，正文/revision 未变；ready 版本 SHA256=`3035dd1b1a7a6ac2f17cf64e973148f01fb04bc11ef53fccdc3b3bdadaa20eb1`。其余三文件原样，独立审查只覆盖计划，不替代实现测试/CI/性能/实现审查。

## Decisions so far
以下前三项是用户已确认目标与行为，不表示实现已完成：
- 支持同一运行时中的不同 Stack 大小，优先解决栈池复用限制。
- 未指定大小使用全局默认；显式指定使用请求值，不改写全局配置。
- 保留内存页对齐；不要求调用者只能提交已对齐大小。
- [参数链路与栈池兼容性核查](tickets/parameter-pool-compatibility.md)：静态证据支持一个端到端路径点；已核参数断点、尺寸/保护属性、全池容量与旧小栈兼容，具体技术候选及未验证边界见 Resolution；整图路线确认依据见下一项。
- [确认最小路线与验收出口](tickets/minimal-route-confirmation.md)：本会话展示完整路线、验证及授权边界后，用户在确认工具中明确选择「确认路线，仅建立 draft 路径点」。据此确认单点路线并建立 draft 骨架；该次确认不授权正式计划细化或实现。随后用户 `/wfer-continue plan` 单独授权本轮规划，仍不授权运行时实现、Git 提交或远端动作。

## Not yet specified
- 无待展开的选型迷雾；已知诊断缺口见 waypoint/tasks.md 6-2 与7-3，用户明确接受留缺口归档。历史未定位间歇测试风险保留，不当成已修复。

## Out of scope
- 完整伙伴系统、共享大块内存的拆分/合并，以及通用内存分配器。
- 已创建协程的自动扩栈、缩栈或换栈，运行期修改全局配置以改变已有栈。
- CoPool 按任务分流或独立池改造、OS worker 线程栈大小配置、infra/framework 接入。
- 改变全局默认栈大小、切换到 mmap、跨平台整体验收，以及本轮代码实现或远端写入。

## Lifecycle
- 2026-10-12 17:40:04 +0800 active → archived：用户在看到6-2 STRINGENT_DEBUG基线编译失败、Valgrind未测及已知间歇风险后明确「没事留缺口归档吧」。PR385已合并，main b36f5c4cd4406162c093f4e886ee5cab3915e9c8全量57/57、性能六模块与Docs通过；保留6-2/完整7-3未完成，proposal仍blocked，不标全部通过。（confirmed=true）

## Archive
- 2026-10-12 17:40:04 +0800 归档（archive=incomplete）：用户在看到6-2 STRINGENT_DEBUG基线编译失败、Valgrind未测及已知间歇风险后明确「没事留缺口归档吧」。PR385已合并，main b36f5c4cd4406162c093f4e886ee5cab3915e9c8全量57/57、性能六模块与Docs通过；保留6-2/完整7-3未完成，proposal仍blocked，不标全部通过。
