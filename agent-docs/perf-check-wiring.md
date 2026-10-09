# perf-check-wiring —— `性能回归检查` 受控报告消费接线（Issue #50，本地候选）

**状态：** 本地候选（未 commit / 未 push / 未建 PR / 未触发 CI）。远端写入由父级执行。
**范围：** 只新增「hosted 性能 job 消费 server3 可信控制端发布的受控报告评论」这一最小接线，
外加一个 stdlib-only 消费脚本与其聚焦测试。**不改** required 集合、`release_gate.py` 行为、
ruleset、release.yml、平台配置或任何正式 baseline。

## 1. 背景与目标

`.github/workflows/unit_test.yml`（workflow path 与名 `CI` 由 `scripts/release_gate.py::validate_main_ci`
按 path 精确读取）此前**不含** `性能回归检查`（app 15368），导致该 required context pending、
Release fail-closed。本轮把它接线为一个**便宜的、fail-closed 的受控报告消费者**：hosted job
**不再重放** unified_stress、**不回退**旧 arc-s4、**不写**任何性能 PASS，只核验外部可信报告。

## 2. 信任边界（本方案的安全核心）

| 边界 | 约束 |
|---|---|
| 凭据 | CI 只用 `github.token`（job `permissions: contents: read`）；**无** repo 写权限、**无** secrets 透传、**无** server3 凭据。顶层 `permissions: {}` 不变。 |
| 控制端 | server3 采样与报告发布在**可信控制端**完成；hosted 只**消费**。父级另行部署/验收并 POST 评论。 |
| 身份 | 报告评论 `author.id` 必须等于固定拥有者 API id **78525443**（exact 整数比较，**不比 login**）。伪装/同名评论被**忽略**，不因此拒绝整个 thread。 |
| 数据 | 评论正文只作 `json.loads`，**绝不执行**；消费脚本只依赖 stdlib `json`/`urllib`/`hashlib`/`base64`，无 `subprocess`/`eval`/`exec`。 |
| 传输 | 绑定 commit 的 **commit comments**（`GET /repos/{repo}/commits/{sha}/comments`）：PR 与 push 通用、无需新 issue/跨仓。分页/条数/字节均有界。 |
| 基线 | 只认已批准 ref `refs/heads/perf-baseline` 上、commit 绑定、字节 sha256 匹配的 baseline 文件；不可比即失败（不置绿）。 |

## 3. subject 与事件

- PR：`PERF_SOURCE_SHA = github.event.pull_request.head.sha`（**真实 head**，不冒充 PR merge commit）。
- push main：`PERF_SOURCE_SHA = github.sha`。
- 事件面显式限定 `pull_request`/`push`（`if:`）。
- **更换 head 即更换 subject**：新 head 未经批准不会产出报告，job 因而失败——这是预期，不是缺陷。
  原 head `5e201bc…` 的批准只对 `5e201bc…` 有效，**不**外推到新 head。

## 4. 报告契约（`bbtools-server3-perf/v1`）

评论正文含固定标记 `<!-- bbtools-server3-perf/v1 -->` 与一段 ```json 报告；报告自带 `schema` 标记。
报告字段（数据闭集，全部硬校验）：

```json
{
  "schema": "bbtools-server3-perf/v1",
  "schema_version": 1,
  "repo": "<owner/repo>",
  "source_sha": "<40-hex，必须等于 subject>",
  "baseline": {"ref": "refs/heads/perf-baseline", "commit": "<40-hex>",
               "path": "tests/baselines/<slug>/<file>.json", "sha256": "<64-hex>"},
  "environment": {"agent": "...", "node": "...", "cpu_model": "...",
                  "logical_cores": 0, "mem_total_kb": "...", "compiler": "...",
                  "cmake_version": "...", "ninja_version": "...",
                  "build_type": "Release", "cmake_args": [], "threads": 2},
  "parameters": {"threads": 2, "dur": 45, "gate_enabled": true},
  "modules": {"comutex": {"ops_total": 0, "ops_per_sec": 0.0, "errors": 0,
                          "elapsed_s": 45.0, "status": "PASS"}, "... 共六个 ...": {}},
  "measurement_sha256": "<64-hex：对不含本字段的报告做 sort_keys+紧凑分隔符的 sha256>",
  "verdict": "PASS|WARN|FAIL|NO_COMPARABLE_BASELINE|METRIC_INVALID|UNSTABLE"
}
```

六模块闭集：`comutex, corwmutex, cocond, chan, copool, coroutine`（集合必须精确相等）。

## 5. 判定链（全部硬条件，任一失败 → 非零）

1. 顶部参数：`source_sha` 必须 40-hex；`expected-author-id` 必须正整数；`repo` 必须 `owner/repo`。
2. 取该 source SHA 的 commit comments（有界分页/条数/字节）。
3. 只保留 `author.id == 78525443` **且** 含标记、`repo`/`source_sha` 匹配的评论；其余忽略。
4. 无匹配 → 有界等待（`--attempts 4 --retry-delay 180`，总时长 < job `timeout-minutes: 15`）后
   `REPORT_MISSING` **failure**。
5. 同源多条有效评论若规范摘要不一致 → `CONFLICTING_REPORTS` failure。
6. `measurement_sha256` 必须等于报告规范序列化的 sha256（防篡改）→ 否则 `REPORT_MALFORMED`。
7. `baseline.ref` == 已批准 ref；（若配置 `vars.PERF_BASELINE_COMMIT`）`baseline.commit` == 已固定 commit；
   `baseline.path` 必须在 `tests/baselines/` 前缀内且无 `..`/绝对路径/反斜杠。
8. 取 `baseline.commit:baseline.path` 文件字节，sha256 必须 == `baseline.sha256` → 否则 `NO_COMPARABLE_BASELINE`。
9. 用该 baseline 真实 JSON **独立复算**：环境指纹（`FINGERPRINT_KEYS`）任一不一致 → `NO_COMPARABLE_BASELINE`；
   六模块须齐全、`errors == 0`、`elapsed_s > 0`、基线 `ops_per_sec > 0` → 否则 `METRIC_INVALID`/`NO_COMPARABLE_BASELINE`。
10. 声明 `verdict` 不得比实测更轻：`severity(declared) >= severity(derived)`（控制器可因延迟/冻结等加重，
    **绝不减轻**）→ 违反即 `METRIC_INVALID`。

## 6. 成功语义与退出码

- **成功（exit 0）**：`verdict ∈ {PASS, WARN}` 且上述 1–10 全通过。`PASS` 打 `::notice::`；
  `WARN` 打 `::warning::`（沿用既有 perf_contract：10% WARN / 20% FAIL、CoCond 30%/40%，
  `--gate-enabled` 与现役 workflow 一致，本轮**从旧现役 workflow 取真**，不新造）。
- **失败（exit 2，fail-closed）**：`FAIL` / `NO_COMPARABLE_BASELINE` / `METRIC_INVALID` /
  `UNSTABLE` / `REPORT_MISSING` / `REPORT_MALFORMED` / `CONFLICTING_REPORTS` / 参数非法。
  **缺报告不置绿**：不用 `skipped`/空成功冒充成功。

## 7. 与既有契约的关系（未改）

- `scripts/release_gate.py::validate_main_ci` **未改**：仍按 `unit_test.yml` 的 main push run 读
  `编译 & 单元测试` 与 `性能回归检查` 两个 success job。报告不存在时 `性能回归检查` 为 failure，
  Release 继续 fail-closed。**required 集合本轮不改**（ruleset 1095939 仍含两者，app 15368）。
- job 名/`app 15368`/`Release` 兼容：job 仍是普通 GitHub Actions job（自动 app 15368）、名精确
  `性能回归检查`、位于 `unit_test.yml`；不引入外部 check 冒充。
- 普通 CI 其余 job（`变更类型检测`/`分类/计划`/`编译 & 单元测试`/`结果汇聚`）与 callee 汇聚契约不变；
  性能 job **不进** framework result 汇聚（独立 context，由 Actions 直接产出）。

## 8. 本地验证（真实结果）

```bash
cd <candidate-worktree>
export PYTHONDONTWRITEBYTECODE=1
TMPDIR=/tmp python3 scripts/ci/test_perf_report_ingest.py          # 26/0
TMPDIR=/tmp python3 -m unittest discover -s scripts/ci -p 'test_*.py'   # 250/0（无 PyYAML：结构契约 SKIP）
TMPDIR=/tmp /usr/bin/python3 -m unittest discover -s scripts/ci -p 'test_*.py'  # 250/0（含结构契约）
TMPDIR=/tmp python3 scripts/test_release_gate.py                  # 12/0
```

> `TMPDIR=/tmp` 仅为本机 scratch 落在 `$HOME` 下时 `run_smoke` 的既有路径校验所致；与本次改动无关。

真实只读探针（联网、只 GET）：

- `GET /repos/yqm-307/bbtools-coroutine` → `owner.id = 78525443`；`GET /users/yqm-307` → `id = 78525443`。
- `GET /repos/yqm-307/bbtools-coroutine/commits/5e201bc…/comments` → `0` 条 → `perf_report_ingest`
  真实运行 `exit 2`（`REPORT_MISSING`）。**父级注入真实报告之前，负例不会假写正式 PASS。**

## 9. 未覆盖 / 残余（诚实边界）

- 任何**真实 CI run**：真实评论读取、真实 baseline 文件字节拉取、真实 check 状态——父级注入评论后连通。
- **控制端部署与评论发布**（server3 采样、报告生成、POST + readback）：本候选不含，属父级。
- 正式 baseline 的**内容**：当前已知 `refs/heads/perf-baseline@564e9a1d…`；父级正以现役入口
  （`c704458`）做 45s×6×3 fresh 采样，新 baseline commit 后应写入 `vars.PERF_BASELINE_COMMIT`。
- `vars.PERF_BASELINE_COMMIT` 未设时**不**锁定 commit（仍强制文件摘要+环境可比+复算）；建议父级设值。
- docs-only PR 目前**没有**性能报告 → 该 context 失败（不回退旧「空成功直通」写法，那会伪造 green）。
  是否给 docs-only 单独豁免需父级/授权决定，本轮不擅自实现。
- 消费脚本内联了 perf_contract 的阈值与指纹键（为满足「stdlib-only、不 import 仓库源码」），
  由 `ContractDriftGuardTest` 断言与 `perf_contract.py` 真源一致，防静默漂移；此复制需在
  perf_contract 变更时同步。
