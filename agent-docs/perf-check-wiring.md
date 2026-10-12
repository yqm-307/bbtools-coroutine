# perf-check-wiring —— `性能回归检查` 同 runner base/head 自动性能门禁（本地候选）

**状态：** 本地候选（未 commit / 未 push / 未建 PR / 未触发 CI）。远端写入由父级执行。
**范围：** 把 `性能回归检查`（app 15368，required context）从「等待 server3 外部 commit comment 的
受控报告消费者」替换为**同一 hosted runner 上自动构建并测量 base+head 的真实比较**。**不改**
required 集合、`scripts/release_gate.py`、ruleset、`release.yml`、正式 `perf-baseline`、仓库变量或
任何产品/runtime 代码。

## 1. 背景与目标

`性能回归检查` 原先 fail-closed 地等待可信控制端发布的 commit comment，报告未产出时该 context 永远
failure，普通 PR 因 required 而不满足合并资格。本轮改为**自给自足**：在同一个 hosted job 内顺序构建
base 与 head 两棵隔离源码/构建树，用**同一份 driver**（head 的 `scripts/ci_perf_check.py`）分别测量
六模块 45s × 2 threads，再以 base 报告为 runner 临时基线对 head 做**严格**比较。这样每次 PR/push 都
能产出真实的同机 base↔head 相对退化结论，不依赖外部报告产出。

## 2. 信任边界

| 边界 | 约束 |
|---|---|
| 凭据 | job `permissions: contents: read`；顶层 `permissions: {}` 不变；**无** secret / OIDC / 写权限 / secrets 透传。 |
| 事件 | 只 `pull_request` / `push`（`if:` 显式限定）；**无** `pull_request_target` / `workflow_dispatch`。 |
| runner | 全部 `runs-on: ubuntu-24.04`；**无** `self-hosted` / `arc-s4`。 |
| 远程 `uses` | 全部完整 40-hex SHA pin；`actions/checkout` `persist-credentials: false`。 |
| 源码 | 两棵源码树按**显式完整 SHA** checkout（`fetch-depth: 0`），并核 `git rev-parse HEAD` 等于期望 SHA。 |
| 基线 | base 报告只作 **runner 临时基线**；**不**读/写 `tests/baselines/`，**不**动 `perf-baseline` 分支，**不**改仓库变量。 |
| 产物 | **无** artifact / cache / 新增服务 / 新增依赖。 |

## 3. subject / base 与 SHA 校验

- subject：PR = `github.event.pull_request.head.sha`（真实 head，不冒充 merge commit）；push main = `github.sha`。
- base：PR = `github.event.pull_request.base.sha`；push main = `github.event.before`。
- **不用** `HEAD^` 猜测。硬校验三项：完整小写 40-hex、非零（`0000…` 首推直接拒绝）、checkout 后
  `git cat-file -e <sha>^{commit}` 可达且 `HEAD == <sha>`。任一失败 → 该 step `exit 2` → job failure。
- 报告用 `--commit-sha` 绑定**各自**源码的完整 SHA：base 报告 `commit=base.sha`，head 报告
  `commit=head.sha`（不再回退 `git rev-parse --short HEAD`）。

## 4. 判定链（同一 runner，顺序执行）

1. 校验 subject/base SHA（§3）。
2. 顺序 checkout `perf-src-base`、`perf-src-head`（各 `fetch-depth: 0`），核验 SHA 绑定。
3. `prepare_boost.sh` 复现 Boost 1.90 `context` 私有前缀（沿用既有固定 SHA256 配方）。
4. 顺序构建两棵树：`cmake -S <src> -B <build> -DNEED_BENCHMARK=ON -DNEED_TEST=OFF` +
   `cmake --build <build> --target unified_stress`。**只构建 `unified_stress` 目标，不跑全量 CTest。**
5. **base 无基线测量**（`--build-dir perf-build-base --commit-sha <base> --no-baseline-compare`）：
   六模块 45s×2threads；verdict = `NO_COMPARABLE_BASELINE`（exit 0）。**这不是 PASS 声明**，只作为
   head 比较的 runner 临时基线；任一模块测不干净（timeout/crash/zero ops/errors>0/缺字段）base 即
   exit 2，整个 job failure。
6. **head 严格比较**（`--build-dir perf-build-head --commit-sha <head> --baseline perf-base-report.json
   --strict-comparable --gate-enabled`）：以 base 报告为基线，逐模块按 ADR D6 阈值判定，且
   `--strict-comparable` 使**任一** `NO_COMPARABLE_BASELINE` 模块（无基线/基线损坏/环境不可比/
   缺 base 模块/基线零吞吐）也 `exit 2`——**缺证据不得置绿**。

## 5. 退出码与成功语义

- base：`NO_COMPARABLE_BASELINE` → exit 0（**仅**表示「无基线不阻塞」，不是通过）。
- head（`--strict-comparable --gate-enabled`）：
  - `PASS` / `WARN` → exit 0（WARN 走 `::warning::` 注解与 Step Summary，不伪装 PASS）；
  - `FAIL` / `METRIC_INVALID` → exit 2；
  - **任一模块 `NO_COMPARABLE_BASELINE`** → exit 2（严格模式）；
  - `errors>0`、zero ops、NaN/inf、缺必需字段、环境指纹不一致均在测量/比较层被拒。
- 旧契约保留：不传 `--strict-comparable` 时 `NO_COMPARABLE_BASELINE` 仍 exit 0（供 base 与既有调用方）。

## 6. 与既有契约的关系（未改）

- `scripts/release_gate.py::validate_main_ci` **未改**：仍按 `unit_test.yml` 的 main push run 读取
  `编译 & 单元测试` 与 `性能回归检查` 两个 success job。job 名/`app 15368`/workflow path 均保持。
- `perf-regression` job **不进** framework result 汇聚（`needs` 为空，独立 context）。
- 普通 CI 其余 job（`变更类型检测`/`分类/计划`/`编译 & 单元测试`/`结果汇聚`）与 callee 汇聚契约不变。
- 复用未改脚本：`scripts/ci/prepare_boost.sh`、`scripts/ci/ci_common.py`、`scripts/ci/perf_contract.py`。
- **`scripts/ci/perf_report_ingest.py` 现不再被 workflow 引用**（原报告消费路径被本方案替换）。该脚本
  与其测试仍在树内且自测通过，本轮**未删除**（超出本候选授权范围）；是否退役由父级决定。

## 7. `ci_perf_check.py` 的最小改动（共同根因 + 同 runner 支持）

- `run_benchmark`：**新增** `errors>0 → {"error": "nonzero_errors"}`。这是共同最小根因——errors>0 是
  操作失败而非有效吞吐样本，旧实现把它当有效测量放行（仅补跑冻结检测，检测为 `ok` 就保留原判定，
  可能 PASS），会让坏指标写进基线/让门禁假绿。修在测量层后，base/head 与所有消费者一致 fail-closed；
  冻结/进程失败诊断移到错误分支，仅补充 `reason`，不改变 FAIL 判定。
- 新增 `--build-dir`：指定被测构建树（默认 `<repo>/build`）。
- 新增 `--commit-sha`：完整 40-hex 绑定被测源码，非法值在任何测量前 `exit 2` 且不产报告。
- 新增 `--strict-comparable`：任一模块 `NO_COMPARABLE_BASELINE` → `exit 2`。
- 其余（`--baseline`/`--environment-file`/`--output`/`--dur`/`--threads`/`--gate-enabled`）复用不变。

## 8. 本地验证（真实结果）

```bash
cd <candidate-worktree>
export TMPDIR=<alice-scratch> PYTHONDONTWRITEBYTECODE=1 HOME=<isolated-home>

# 聚焦：perf 契约与新增回归
python3 -m unittest scripts.ci.test_perf_contract           # 79/0
# 全量发现（无 PyYAML → 结构契约显式 SKIP）
python3 -m unittest discover -s scripts/ci -p 'test_*.py'   # 268 OK, 23 skipped
# 全量发现（/usr/bin/python3 有 PyYAML → 结构契约实跑）
/usr/bin/python3 -m unittest discover -s scripts/ci -p 'test_*.py'  # 268 OK, 14 skipped
python3 scripts/test_release_gate.py                        # 12/0
```

> `HOME` 隔离：`run_smoke` 的既有路径守卫会拒绝「`--build-dir` 位于 `$HOME` 下但不在 `--source-dir` 内」，
> 当 scratch 落在 `$HOME` 下时该既有用例会误红；用隔离 `HOME`（或 `/tmp`）即可，与本次改动无关。

同 runner 端到端冒烟（真实脚本 + 极小假二进制，不重复大型构建）：

| 场景 | 命令要点 | 结果 |
|---|---|---|
| base 无基线测量 | `--build-dir base --commit-sha <base> --no-baseline-compare` | rc=0，verdict `NO_COMPARABLE_BASELINE`，`commit=base.sha`，6 模块 |
| head 严格比较（-5%） | `--build-dir head --commit-sha <head> --baseline base.json --strict-comparable --gate-enabled` | rc=0，verdict `PASS`，`commit=head.sha`，`comutex delta≈-4.955%` |
| head `errors>0` | 同上 + 假二进制报 `errors=3` | rc=2 |
| head 缺 base 模块（strict） | base 报告删去 `chan` | rc=2 |
| 同上但**不**加 `--strict-comparable` | 旧契约 | rc=0 |

workflow 静态校验：PyYAML 解析通过（jobs = `{changes, plan, build, perf-regression, result}`、
perf `timeout-minutes: 60`、`permissions: {contents: read}`、无 `needs`）；`_extract_run_blocks`
抽出的 **15** 个 run 块全部 `bash -n` 通过；`require_sha` 对合法 40-hex 返回 0，对空/短/大写/非 hex/
零 SHA 均返回 2。

## 9. 未覆盖 / 残余（诚实边界）

- **任何真实 hosted run**：Boost 下载校验、两棵树真实 cmake/ninja 构建、六模块真实 45s×2 测量、
  check 状态——本地只做静态契约 + 假二进制端到端，真实链路由父级 PR CI 连通。
- **`vars.PERF_BASELINE_COMMIT` / 正式 baseline / 控制端**：本方案**不涉及**；server3 采样与报告发布
  路径本轮不接线。
- **fork PR 的 head SHA**：`actions/checkout ref: head.sha` 在**同仓** PR（本仓常态）可达；若某 fork
  的 head commit 在 base 仓不可达，checkout/核验会 fail-closed 失败（符合「不可达即失败」，但会阻塞该
  fork PR）。是否放宽（改用 `refs/pull/N/head`）需父级决定。
- **`perf_report_ingest.py`**：已不被 workflow 引用但保留在树内（见 §6）。
- **`scripts/test_release_gate.py`**：其中 `test_gate_layering_perf_is_bounded_hosted_measurement`
  （原 `…keeps_merge_path_cheap`）直接读 workflow 文本守卫，**必须**随本次接线同步更新才能全绿；该文件
  不在父级给的允许清单内，属**必要一致性更新**，请父级复核/接受。

## 10. 设计边界（ponytail ceiling）

同 runner 短测是**廉价、自足的 base↔head 同机相对退化门禁**，**不是** server3 长期性能基线：它只
回答「同一台机器上 head 相对 base 是否退化」，不提供跨机可比性、长期趋势或绝对基线。长期/跨机趋势
仍由 `record_baseline.py` + `perf-baseline` 分支承担（本轮不接）。若要升级为长期基线，应把 base 报告
按批准流程写入正式 baseline 并在 server3 上复采——那是后续独立工作，不在本候选内。
