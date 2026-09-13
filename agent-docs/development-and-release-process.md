# bbtools-coroutine 开发与发布流程

**状态：** 当前流程真源；远端保护状态必须以 GitHub API 实时核对
**适用范围：** 所有 Agent 开发、PR、里程碑收口和版本发布
**硬闸门：** GitHub Actions required checks、main branch ruleset、`v*` tag ruleset；未核对远端配置前不得视为已生效

## 1. 目标与职责

Agent 负责在明确范围内自主完成：读取需求和契约、分析调用关系、实现、测试、创建 PR、修复业务代码导致的 CI 失败、回写证据。修改 CI/CD 配置、GitHub Actions、部署脚本或构建入口仍需用户确认；用户负责里程碑目标、计划设计、重大架构取舍、破坏性变更、进入受控使用期、RC 发布授权和 Stable 发布决策。

文档不是安全边界。任何 Agent 规则都不能绕过 GitHub branch protection、required checks、tag ruleset 和 release workflow。

## 2. 开发流程

```text
Issue / 里程碑计划
  -> 读取契约、相关 Issue/PR、当前实现和测试
  -> 创建工作分支
  -> 最小实现 + 回归测试 + 必要文档
  -> 本地构建与验证
  -> 创建 PR，目标 main
  -> CI required checks 全部通过
  -> Review 通过
  -> 合入 main
  -> main 集成验证
```

### Agent 必须遵守

- 核心运行时任务先读取 `agent-docs/2026-09-07-core-runtime-contract.md`。
- 需求、决策和验收依据先读取对应 GitHub Issue/PR。
- 不直接推送 `main`。
- 不把本地结果说成远端 CI 结果。
- 不把 WARN 说成 PASS。
- 不把部分压测、被取消压测或缺少汇总的压测说成完成。
- 新测试必须注册到 CTest；行为变更必须留下可重复验证入口。
- PR 必须关联 Issue，并写明范围、验证命令、结果和未覆盖边界。
- CI 失败先读取日志、定位根因，再修改；业务代码修复可自主执行，CI/CD 配置修改必须先获确认；不靠重复重跑掩盖失败。
- 开 Issue / PR 用 `.github/ISSUE_TEMPLATE/task.md` 与 `.github/pull_request_template.md`。验证按下表选现有命令，不另起流程。

### 验证阶梯

| 改动类型 | 本地最低验证 | 合入仍走的 PR Checks | 不要在普通 PR 当完成证据 |
|----------|--------------|----------------------|--------------------------|
| 仅文档 / 模板 / gitignore | `git diff --check`；抽查链接 | `编译 & 单元测试`、`性能回归检查` | 真实客户端、疲劳 |
| `scripts/` 或 `release_gate.py` | `python3 scripts/test_release_gate.py` 或对应 `scripts/ci/test_*.py` | 同上 | 发布 Gate、dispatch RC |
| 单测 / example | 相关 `ctest` 或该 example | 同上 | 1h soak |
| 运行时 / 公开 API | 全量 `ctest --test-dir build --output-on-failure` | 同上 | 真实客户端 |
| Hook / 兼容行为 | 相关 hook 单测 + 全量 ctest | 同上 | #256/#284 长测与发布 Gate |
| 压测程序本身 | 短跑或相关 benchmark | `性能回归检查` | 未授权的长 soak |

本地命令见 §9。普通 PR 不把 `scripts/acceptance_real_clients.py`、1h `run_fatigue.py` / `run_parallel_stress.sh` 当合入完成证据。

### PR required checks

PR 快速反馈，main 快速集成，Release 严格发布。普通改动先自审并运行相关测试；独立审查按风险或明确要求执行，不因非阻塞措辞调整重复整树复审。

以下检查是 PR 合入 main 的硬门槛（均为分钟级）：

- `编译 & 单元测试`
- `性能回归检查`（unified_stress 45s/模块快速回归）

`性能回归检查` 对无基线、基线损坏或环境指纹不一致保持显式 `NO_COMPARABLE_BASELINE`（warning），但不能宣称性能通过；版本发布要求可比基线。性能基线只在发布 Gate 完整长测通过后写入（见 §5），禁止手改指纹以伪造可比性。

`真实客户端验收` 与长时疲劳压测不在普通合并路径执行，只在发布 Gate 执行（见 §5）。发布 Gate 的 `真实客户端验收` 必须在明确的 Redis 环境中执行。Redis 不可用时，发布 Gate 失败；本地脚本允许用 CTest skip code 77 表示环境缺失，但不能把 skip 当作发布通过。

## 3. main 集成流程

每次合入 main 后，CI 只运行分钟级集成验证：

- 编译与全量 CTest、Smoke、Reliability
- 快速性能回归检查

发布级检查（真实客户端验收、可配置时长的六模块并行疲劳压测、性能基线记录与趋势检查）不在 main 路径执行，由 §5 的发布 Gate 承担。main 的定位是快速、持续可集成；发布资格由发布 Gate 证明。

压测必须满足以下条件才可形成通过证据：

- 运行到计划时长；
- 六个模块均有最终有效指标；
- `errors=0`；
- 无 timeout、crash、zero ops、missing summary；
- 汇总文件真实生成；
- 结果绑定当前 commit SHA。

被取消、收到 SIGTERM/SIGKILL、runner 无响应或只生成部分日志的运行只能标记为未完成，不得写入新基线。

## 4. 版本策略

当前历史正式版本为 `v2.1.0`。M1 改变核心运行时语义，采用主版本升级：

```text
v3.0.0-rc1 -> v3.0.0-rc2（必要时） -> v3.0.0
```

- `rcN`：发布候选，只能作为 prerelease，进入受控使用期。
- Stable：只能从已验证 RC 的同一 commit 晋级。
- 版本身份只认不可移动的 Git tag；不额外维护容易漂移的版本号。
- 首发产物为 GitHub Release + 源码包；暂不引入 CPack、Conan、vcpkg 或新包仓库。

## 5. RC 发布流程

由 `.github/workflows/release.yml` 的 `workflow_dispatch` 触发，输入：

- `release_kind=rc`：只能由用户授权触发；Agent 可准备输入和证据，但不得自行发布 RC。
- `version=v3.0.0-rc1`
- `source_sha=<main 的完整 SHA>`
- `soak_seconds=<疲劳压测秒数，默认 3600，范围 60-86400>`：发布前可提高到数小时-数十小时；长测只在发布 Gate 执行

RC Gate 必须验证：

1. 版本格式合法且 tag 尚不存在；
2. `source_sha` 是当前远端 main HEAD，且该 SHA 的 main 集成 CI（分钟级作业集）通过；
3. Release 构建成功；
4. 全量 CTest、真实客户端验收通过；
5. 按 `soak_seconds` 完整跑满六模块并行疲劳压测；
6. 六模块性能判定均为 `PASS` / `WARN` 且错误数为零；`NO_COMPARABLE_BASELINE` 不能作为发布资格；
7. 发布摘要绑定 source SHA；
8. Gate 通过后记录性能基线并推送 `perf-baseline` 分支（该步骤失败即 Gate 失败）。

全部通过后，workflow 创建不可移动的 RC tag 和 GitHub prerelease。任一 Gate 失败，不创建 tag 和 Release。

发布 Gate 的 `gate` job 需要 `contents: write`（仅用于推送 `perf-baseline` 基线）；发布凭据（`v*` tag 的 deploy key）仍只在经过 Environment 审核的 `publish` job 中使用。

## 6. Stable 发布流程

由同一个 workflow 触发，Stable 只能由用户决定并在 `release-stable` Environment 审核通过后继续，输入：

- `release_kind=stable`
- `version=v3.0.0`
- `source_sha=<RC 的完整 SHA>`
- `rc_tag=v3.0.0-rc1`

Stable Gate 额外验证：

1. RC tag 存在且指向 `source_sha`；
2. RC Release 已存在并标记为 prerelease；
3. Stable 版本与 RC 的 major / minor / patch 完全一致；
4. source SHA 与 RC SHA 完全一致；
5. RC 受控使用期没有未处理的 Critical/Important 阻塞，由用户确认对应 Issue/验收记录；
6. 重新执行发布回归并生成摘要。

通过后创建 Stable tag 和正式 GitHub Release。Stable 不得从另一个未经 RC 验证的 commit 直接生成；有新代码必须先发布新的 `rcN`。

## 7. 远端硬保护

`main` 必须配置：

- 禁止直接 push；
- 禁止 force push 和删除；
- PR 必须基于最新 main；
- required checks 为 `编译 & 单元测试` 与 `性能回归检查`（均为分钟级）；发布级检查不作为合并门禁。

`v*` 必须配置 tag ruleset：

- tag 创建只允许 release workflow；
- tag 创建后不可更新或删除；
- Stable 和 RC 都必须不可移动。

如果仓库平台能力不能完整表达上述规则，必须在 release workflow 中再次验证，不得以文档替代硬保护。

GitHub tag ruleset 的“限制创建”按 bypass actor 授权，不能直接选择某一个 workflow。本仓库实现（2026-09-09）：ruleset `protect version tags`（`refs/tags/v*`）包含 `creation` / `update` / `deletion` 三条规则，唯一 bypass actor 是发布专用 deploy key `release-tag-pusher`；release workflow 的 publish job 用该 key 推送 lightweight tag，再由 Releases API 创建 Release（tag 已存在，Release 关联既有 tag）。私钥只存于仓库 secret `RELEASE_TAG_SSH_KEY`，`release_gate.py publish` 缺失时 fail closed。双向实测：管理员凭据推送被拒（`Cannot create ref due to creations being restricted`），deploy key 创建与删除被放行（`Bypassed rule violations`）。残余风险：deploy key 具有仓库写权限（可推送任意分支；`main` 仍受 `ban push` ruleset 保护且无 bypass），如需更小权限面可后续换成仅授予 contents:write 的专用 GitHub App。Stable 发布还必须绑定 GitHub Environment `release-stable` 的人工审核者。

发布前必须使用 GitHub API 回读两个 Environment 的 `required_reviewers`、`prevent_self_review`、部署分支策略和 `can_admins_bypass`；仅有 Environment 名称不算审核门禁，缺失时 GitHub 会自动创建无保护环境。无法确认配置时不 dispatch。

当前边界（2026-09-09 回读）：仓库公开；两个 Environment 均有唯一审核者 `yqm-307`，允许同账号触发后审核（`prevent_self_review=false`）。发布仍需用户授权，不要求另设账号。Environment 的 `can_admins_bypass=true`，管理员仍可旁路 Environment 审核；`v*` 规则的创建已收紧到发布 deploy key（见上），管理员凭据无法创建、更新或删除版本 tag。

公开仓库的 `pull_request` 执行的是 PR head 中的 workflow 文件；常驻 self-hosted runner 上，YAML 内的 `if` 无法阻止 fork 替换 workflow 后请求同一 runner。仓库 Actions `approval_policy` 已设为 `all_external_contributors`（API 回读），`GITHUB_TOKEN` 默认 `read` 且禁止用它批准 PR review。外部 fork workflow 必须由有写权限的人批准才会跑；Agent 和用户都不得批准未审查的 fork workflow 到该 runner。有写权限的人一旦批准，隔离仍不成立。完整隔离只能改为私有仓库，或撤销本仓库的 self-hosted runner。上述配置不等于发布闭环完成。

## 8. 证据与失败处理

发布证据至少包含：source SHA、版本、构建类型、编译器、测试结果、真实客户端结果、压测汇总、性能判定、未覆盖边界和 workflow run URL。

发布失败时：

- 保留失败 run 和日志引用；
- 发布前 Gate 失败不创建 tag 或 Release；
- 创建请求返回不确定或回读失败时，先查询远端 tag/Release 对账，禁止盲目重试；
- 已创建的 tag 不移动、不删除；
- 修复后递增 RC 序号；
- 不关闭 Issue，直到验收证据支持关闭。

## 9. 本地入口

```bash
# PR 基础验证
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DNEED_BENCHMARK=ON -DNEED_EXAMPLE=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure

# 真实客户端验收
python3 scripts/acceptance_real_clients.py \
  --echo-server ./build/bin/example/echo_server \
  --hiredis ./build/bin/example/co_with_hiredis

# 发布前本地模拟（不创建远端 tag/Release）
python3 scripts/release_gate.py validate-version --version v3.0.0-rc1
```

本地验证不能替代 GitHub required checks；远端 release workflow 是版本发布唯一入口。
