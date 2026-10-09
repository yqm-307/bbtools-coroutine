# formal-ci —— bbtools-coroutine hosted 普通 CI 正式候选（Issue #50）

**状态：** 本地候选（未 commit / 未 push / 未建 PR / 未触发 CI）。远端写入与 PR 由父级执行。
**阶段限定：** 本候选只把**普通 CI 的 Layer 1** 与 **docs-check** 迁到 hosted，并把影子入口退役；
**旧普通入口中的性能 job 已移除，新可信性能入口尚未接通**；required 集合、Release 执行逻辑、memcheck、runner 规则均未改。在获准合并与远端核对
完成前，本文不构成「迁移已完成」的结论。

## 1. 做了什么

- 把已合并并在线跑通的 hosted Layer1 影子（`.github/workflows/ci-shadow-v1.yml`）**升为唯一正式
  `.github/workflows/unit_test.yml`**；影子入口删除（不双跑）。
- workflow 名保持 `CI`、workflow path 保持 `.github/workflows/unit_test.yml`——`scripts/release_gate.py`
  的 `validate_main_ci` 按该 path 精确读取 `push main` run 与两个 success job，不能转到别的 path。
- 普通触发面：`pull_request → main`、`push → main`。**不新增** `workflow_dispatch`（旧 sanitizer
  长测依赖它；本轮不开启新的长期执行）。
- 全部本地 job 固定 hosted `ubuntu-24.04`；顶层 `permissions: {}`，仅需 checkout 的 job 授予
  `contents: read`；无 secret / OIDC / 写权限 / secrets 透传；远程 `uses` 全部完整 40-hex SHA pin。
- 复用**已发布 fixed-SHA** 的 framework 分类/结果 reusable callee
  `yqm-307/bbt-framework/.github/workflows/bbtools-classify-v1.yml@1c0b0fb0ebc8e7ca3888aa4f6a74d1baecf08cc7`：
  `plan` 只做 classification（不传 results）；`result` 恒 `always()` 汇聚三个真实 `needs.*.result`（含
  skipped），fail-closed。callee 只做分类/结果，不承担 C++ 构建。
- `docs-check.yml`：generic self-hosted → `ubuntu-24.04`，checkout 完整 SHA pin，保持既有 trigger
  paths / name / `timeout-minutes: 5` / 读权限；修正不实注释；空格与密钥扫描保留且改为 fail-closed。
- `scripts/ci/test_shadow_ci.py` → `scripts/ci/test_formal_ci.py`：移除影子身份与「影子 ↔ 旧
  unit_test.yml」自耦合断言，改为对**实际正式 Recipe** 的不可少守卫；`agent-docs/ci-shadow-v1.md`
  → `agent-docs/formal-ci.md`（本文件）。

## 2. 判据等价（与旧 `unit_test.yml` 的 Layer 1）

| 判据 | 旧 unit_test.yml（Layer 1） | 本候选 |
|---|---|---|
| 构建 | `cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DNEED_BENCHMARK=ON` + `cmake --build . --parallel "$JOBS"`（`JOBS: 3`） | 同左 |
| 工具自测 | `python3 scripts/test_release_gate.py` + `python3 -m unittest discover -s scripts/ci -p 'test_*.py'` | 同左 |
| 全量单测 | `ctest --timeout 60 --output-on-failure`（串行），最多 3 次有界重试；step `timeout-minutes: 10` | 同左 |
| 冒烟 | `timeout --signal=TERM --kill-after=10s 90s bin/unit_test/Test_smoke --log_level=test_suite`，3 次；step 8min | 同左 |
| 可靠性 | `timeout --signal=TERM --kill-after=10s 120s bin/unit_test/Test_reliability --log_level=test_suite`，3 次；step 8min | 同左 |
| 调度/工具链日志 | runner / nproc / ulimit / Cap grep || true | 同左，另采集 cmake/g++/ninja/python 版本 |
| required check 名 | `编译 & 单元测试` | 同左（build job，display name 不变） |

重试为 **3 次有界**，每次 attempt 独立进程、独立日志，失败证据留在 job 日志；总时长由 step 级
`timeout-minutes` 界定。**不写成 first-pass，也不扩重试或放宽检查。**

## 3. 分类 / 结果语义

- `required_checks_json=["changes","plan"]`、`optional_checks_json=["build"]`：互斥、非空（callee 校验）。
- `build` 仅当 `plan.result == 'success'` 且分类不为 `docs-only` 时运行；**docs-only 显式跳过**
  （由 callee 的 optional 语义允许）——不靠空成功 alias 把 job 结果伪造成 success。
- **classifier 失败 / 分类未知 ≠ docs-only**：变更集为空或 classifier_status≠ok 时 callee 判
  `unknown` → 保守执行全部，`build` 的 skipped 会被判 failure，不借 docs 变绿。
- 变更集来自本仓真实 diff（`scripts/ci/changed_files.py`）：PR 三点 diff、push `before..sha`（首推
  回退 `HEAD^..HEAD`）；未知 revision / diff 失败 / 超预算（>3000 文件或 >60KB）一律回退空列表
  → callee 判 unknown → 执行全部。**不接受调用方任意 shell/jobs 输入**，本仓不复制第二套分类契约。

## 4. hosted-only 与工具链

- 全部本地 job 固定 `runs-on: ubuntu-24.04`；不再使用 self-hosted / `arc-s4`（普通事件不再直达旧 ARC）。
- 工具链缺口只有 Boost 1.90：hosted 预装的是系统 Boost（版本不同）。`scripts/ci/prepare_boost.sh`
  按 framework 已审查发布的 `docker/toolchain.lock`（`BOOST_VERSION=1.90.0` /
  `BOOST_SHA256=5e93d582…`）下载并 sha256 fail-closed，**源码编译** `context` 到工作区私有前缀
  （不写 `/usr/local`、不装系统包、不改镜像），经 `BOOST_ROOT` / `CMAKE_PREFIX_PATH` /
  `LD_LIBRARY_PATH` 传入构建与测试步骤。
- hosted 实际 `cmake` / `g++` / `ninja` / `python3` 版本由 job 内「采集 runner 调度与工具链日志」
  步骤读回作证据；本仓 CMake 仅需 `>=3.16`。**实际版本必须由真实 run 读回，本文件不预填。**

## 5. 并发与身份

```
group: CI-${{ github.event_name == 'pull_request' && github.ref || format('run-{0}', github.run_id) }}
cancel-in-progress: ${{ github.event_name == 'pull_request' }}
```

- push/main：group 含 `run_id` → 逐提交独立、互不取消，保住每个 SHA 的 main 结果（Release 消费者
  按 `head_sha` 精确读取）。
- PR：group 用 `github.ref`（`refs/pull/<N>/merge`）→ 只取消同一 PR 的旧 run。
- 普通 check 只有本文件一个 producer（影子已删，非双跑）。

## 6. 明确未完成 / 规则阻断（本轮授权的性能冻结）

- **`性能回归检查` 未迁移、未执行**：旧 `unit_test.yml` 的 perf-regression job（unified_stress
  45s/模块）**不在**本候选内——既不在 hosted 重放，也**不**回退启动旧 `arc-s4` 性能执行；本候选
  **不承载任何 perf 判定，也不构造 PASS**。
- 仓库 ruleset（id 1095939 `ban push`）required 仍为 `编译 & 单元测试` **与** `性能回归检查`
  （app 15368）。本轮**不修改 required**。因此：
  - 普通 PR 的 `性能回归检查` context 会 **pending** → **PR 不满足合并资格**；
  - `scripts/release_gate.py` 的 `validate_main_ci` 要求同一 main push run 同时出现两个 success job，
    缺 `性能回归检查` → **Release fail-closed 拒绝**。
- 上述状态由 `scripts/ci/test_formal_ci.py::FormalRecipeGuardTests
  .test_release_consumer_contract_is_preserved_and_perf_is_frozen` 断言（直接读真实
  `scripts/release_gate.py` 与本 workflow 文本），防止后人误把本候选当作「整体已就绪」。
- **Sanitizer（ASAN+UBSAN）长测**：旧 `unit_test.yml` 的 `sanitizer-check` 需要 `workflow_dispatch`
  才可达，而旧 `on:` 从未声明 workflow_dispatch，故该 job **实际不可达**、不属于普通适用测试。
  本轮不开启新的长期执行，该 job 从本文件移除；配置保留在 git 历史（HEAD c704458 的旧
  `unit_test.yml`）中，**需单独迁移为独立 workflow 后再启用**。
- **实际 context 状态必须以远端为准**：合并后由 GitHub API 回读 PR 的 check 集合与 ruleset
  required 的匹配情况；本文件的静态断言不等于线上证据。

## 7. 流程文档的处置（阶段限定，不代表迁移完成）

- 流程真源 `agent-docs/development-and-release-process.md` 与用户向 `docs/ci-guide.md` **本轮不改**。
  其 runner、Layer 2 与超时描述属于本候选前的流程，不代表新入口仍执行这些任务；维护窗内以本文明确的未迁移状态对账，不据旧描述宣称可合并或可发布。
- `AGENTS.md`、`memery_test_info.yml`、`release.yml`、产品 C++、`CMakeLists.txt`、依赖锁、
  `perf_contract.py`、性能基线路径、required 规则、runner 标签均未改动。

## 8. 本地验证（真实结果）

```bash
cd <coroutine-worktree>
export PYTHONDONTWRITEBYTECODE=1

# (1) 生产等价（默认 python3 无 PyYAML、无 BBT_CI_SHARED_DIR）——全量发现
python3 -m unittest discover -s scripts/ci -p 'test_*.py'

# (2) 全绿本地：PyYAML + 已发布 callee 逻辑（真实复用，不复制契约）
BBT_CI_SHARED_DIR=<framework>/scripts/ci/shared \
  /usr/bin/python3 -m unittest discover -s scripts/ci -p 'test_*.py'
```

运行时区分测试实际执行与显式 SKIP。`scripts/test_release_gate.py` 保留真实发布 guard 的严格验证，并断言缺失/跳过/取消/失败的性能 job 仍被 `validate_main_ci` 拒绝；不再要求普通 workflow 内存在已移除的性能 job。测试属于离线回归，不是发布证据。

分类路由冒烟覆盖（用**同一个真实 callee CLI**）：docs-only / code / classifier 失败=unknown /
空变更=unknown；`docs-only + build skipped → success`；`code + build success → success`；
`code + build failure → failure`；`code + build skipped → failure`；`code + build cancelled → failure`；
`plan failure + build skipped → failure`；`required skipped → failure`；`required cancelled → failure`；
`unknown + build skipped → failure`；未知 job key → 契约拒绝。另有「用实际 workflow format 模板
渲染出的 event_json / results_json 喂给真实 cli.py」的桥接回归（防 literal 反斜杠缺陷复发）。

docs-check 行为负例（真实 bash）：SHA 格式非法/缺失 → 拒绝（exit 2）；范围 rev 不可达 → 失败；
真实尾随空白 → 被抓住；干净范围 → 放行；`git grep` 退出码 >1（非 git 仓库）→ fail-closed（exit 2）；
synthetic token（片段拼接，本测试源码不含真实凭据）→ 命中（exit 1）。

## 9. 未覆盖（本地不冒充已验）

- 任何**真实 hosted run**：reusable 跨仓在线身份/权限、hosted 工具链实际版本、Boost 下载与校验的
  真实执行、`编译 & 单元测试` / `结果汇聚` 的真实 check 状态。
- 任何 **C++ 全量构建 / ctest 真实执行**（本地只做 bash -n / 静态契约 / 配方守卫 / 真实 cli 路由 /
  有界 bash 行为负例）。
- **性能路径**（perf job、基线读写、`性能回归检查` context）与其迁移。
- required 集合调整、Release 侧放行、memcheck、runner 采购/准入、生产发布链路。
- 合并后远端对账（PR checks、ruleset 生效状态）。

## 10. 残余差异 / 需注意

1. **docs-only 时 `编译 & 单元测试` 为 skipped**（显式跳过）。旧 unit_test.yml 走「空成功 step」
   使该 check 显示 success；本候选按授权改为显式 skip，并由 callee 的 optional 语义限定**仅
   docs-only** 可跳过。该 context 在 docs-only PR 上是否满足 required 需以远端回读为准。
2. 已发布 callee 内部纯逻辑 job 固定 `ubuntu-latest`（caller 不可改）。
3. hosted `cmake` 版本可能高于旧 ARC 的 `3.28.3`（本仓只需 `>=3.16`）；真实值需 run 读回。
4. Boost 只编 `context`（旧 CI 日志实证的唯一编译/链接组件）。
5. 分类/结果处理器与调用方的身份分离由 callee 自身 `job.workflow_*` 完成；本仓不解析关联 caller。
