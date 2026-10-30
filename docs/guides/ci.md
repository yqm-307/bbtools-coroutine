# bbtools-coroutine CI 说明

本文是 bbtools-coroutine 持续集成的唯一落点文档（原 `docs/ci-guide.md`、
`agent-docs/formal-ci.md`、`agent-docs/perf-check-wiring.md` 已收敛至此）。它描述
`.github/workflows/` 下各 workflow 的组成、门禁清单、判定语义与本地复现命令；
workflow 文件本身是权威定义，本文不复制其步骤，只给门禁索引与语义。

## 1. 组成

| 文件 | 作用 |
|---|---|
| `.github/workflows/ci.yml` | 普通 CI 入口（workflow 名 `CI`）：`pull_request`/`push` 到 `main` 时运行 `changes → docs-check → build-test → perf-regression → result`（perf-regression 仅 perf 位为真时跑） |
| `.github/workflows/ci.yml 的 docs-check job` | 文档类路径上的轻量快速反馈（<5min：空格 diff 检查 + 密钥扫描，fail-closed） |
| `.github/workflows/memcheck.yml` | 内存检测：每周五 17:30（UTC+8）定时 + `workflow_dispatch`，跑在 `arc-s4-memcheck` |
| `.github/workflows/release.yml` | 发布 Gate：`workflow_dispatch` 触发，Release 构建 + 全量 CTest + 真实客户端验收 + 性能 Gate + 可配置时长压测 + 基线写入 |
| `scripts/ci/changed_files.py` | 从真实 git diff 归一受限变更集，完成路径分类（docs-only/code/unknown）并产出 perf 触发位（见 §2） |
| `scripts/ci/result.py` | result job 的本仓内联汇总：needs.*.result → verdict，fail-closed（见 §3） |
| `scripts/ci/prepare_boost.sh` | 按锁定版本/sha256 源码构建 Boost 1.90 `context` 到工作区私有前缀 |
| `scripts/ci/run_memcheck.py` + `build_memcheck_target.sh` | memcheck 环境前置校验、`mem_check_test` 构建与判定（见 §6） |
| `scripts/ci/run_smoke.py` / `run_soak.py` | 冒烟/soak harness（构建 + ctest + 报告落 `tests/reports/`） |
| `scripts/ci/perf_contract.py` | 统一性能报告 schema、环境指纹比较、阈值判定、基线写闸门（见 §5） |
| `scripts/ci/check_docs.py` | docs 检查：相对链接有效、docs 文件名无过程编号、README ≤200 行、ADR 编号唯一 |
| `scripts/ci/check_format_diff.sh` | 只检查本 PR 改动行的 clang-format 违规 |
| `scripts/ci/test_changed_files.py` | 分类规则 + perf 触发位 + result 契约单测（仅标准库） |
| `scripts/ci/test_check_docs.py` | check_docs 正反例单测 |
| `scripts/ci/test_formal_ci.py` | 现役 workflow 的离线冒烟与文本/结构契约测试 |
| `scripts/ci_perf_check.py` | 性能测量 driver：构建树 → 六模块统一 JSON 报告；`--strict-comparable --gate-enabled` 为严格门禁模式 |
| `scripts/record_baseline.py` / `run_fatigue.py` / `run_parallel_stress.sh` | 正式基线记录/趋势检查、串行疲劳、六模块并行压测 |

**分类/结果 callee 链已退役（方案 D3）**：`plan`/`result` 原经
`yqm-307/bbt-framework/.github/workflows/bbtools-classify-v1.yml` 复用完成；
分类规则已由 `scripts/ci/changed_files.py` 承载，结果判定由 `scripts/ci/result.py`
承载；`uses:` callee 接线已随 workflow 重写移除。

## 2. 路径分类与 perf 触发位（changes job）

`changed_files.py` 把真实仓库 diff（PR 三点 `origin/<base>...HEAD`；push
`before..sha`，首推回退 `HEAD^..HEAD`）归一为仓库相对路径列表，并按下列规则分类：

| 路径形状 | 判定 |
|---|---|
| `CMakeLists.txt`（任意目录） | code |
| `*.cmake`、`*.py` | code |
| `.github/workflows/**` | code |
| `docs/` 前缀、`*.md`/图片/`*.txt`、`LICENSE` 等根目录文书 | doc |
| 其余（源码、脚本、锁文件等） | code |

变更集判定：**全部路径为 doc → `docs-only`**；含任一 code 路径 → `code`；空集或分类器
非 ok（diff 失败/未知 revision/超预算：>3000 文件或 >60KB JSON，整集回退不裁剪假子集）
→ `unknown`，保守执行全部。`docs-only` 才允许重型 job（build）显式跳过；
`code`/`unknown` 禁止任何跳过。

**perf 触发位**（`perf` 输出，`true`/`false`）——只有会影响性能测量结论的改动才触发
性能回归检查（方案 D4）：

| 路径 | 触发 |
|---|---|
| `bbt/**` | 被测运行时源码 → true |
| `benchmarks/**` | 被测驱动 `unified_stress` 及其构件 → true |
| `benchmark_test/**` | 历史旧名（已改名 `benchmarks/`），经兼容映射折回新名 → true |
| `scripts/ci_perf_check.py`、`scripts/ci/perf_contract.py`、`scripts/ci/ci_common.py`、`scripts/ci/prepare_boost.sh`、`scripts/record_baseline.py`、`scripts/run_fatigue.py`、`scripts/run_parallel_stress.sh` | 测量/判定/工具链脚本 → true |
| 根 `CMakeLists.txt`（仅根，全局编译选项） | true |
| `tests/`、`docs/`、`examples/`、其他 `scripts/`、`.github/workflows/` 等 | false（不触发） |

分类器非 ok 或变更集为空/超预算时 `perf=true`：保守执行，绝不擅自免测。

## 3. 结果汇聚（result job）

`result.py` 把 `needs.*.result` 汇总为唯一 required check 判定，fail-closed：

- `required` 集合必须非空（空即拒绝，防「全跳过变绿」）；
- 任一 required 为 `failure`/`cancelled`/缺失/`skipped` → verdict=failure；
- results 出现 required∪optional 之外的 job 或非法状态值 → 拒绝；
- optional 非 success 时：`skipped`/缺失仅当该 job 被列入调用方显式声明的
  `expected_skips` 才放行（payload 里没有「按分类自动放行」的语义）；
- 无任何真实 success 不得判绿；
- verdict=failure → 退出码 1；契约拒绝 → 退出码 3。

现役 result job `if: ${{ always() }}` 运行，`needs` 覆盖 `changes`、`docs-check`、
`build-test`、`perf-regression` 四个 job（含 skipped 结果），不构造假状态、不省略键。
heredoc 按 job 触发条件推导 `expected_skips`：

- `build-test` 仅当 `classification=docs-only`（docs-only PR 的显式跳过）；
- `perf-regression` 仅当 `perf=false`（D4 未触发，docs-only 恒成立）或
  `workflow_dispatch`（perf job 的 `if` 限 `pull_request`/`push`，dispatch 必然 skipped）。

**perf 触发时 `perf-regression` 进汇聚且 fail-closed**：skipped/缺失/失败都判红，
不再「独立 context 不进汇聚」。changes 失败时其 outputs 为空，CLASSIFICATION/PERF
显式回退 `unknown`/`true` → 不声明任何预期跳过 → 执行全部。

## 4. 编译与单元测试（build job，`编译 & 单元测试`）

docs-only 显式跳过（optional 语义允许，不靠空成功伪造绿）；code/unknown 必跑，
hosted `ubuntu-24.04`，`timeout-minutes: 75`：

1. **工具链**：`prepare_boost.sh` 按锁定 SHA256 源码构建 Boost 1.90 `context` 到工作区
   私有前缀（hosted 预装 Boost 版本不同），经 `BOOST_ROOT`/`CMAKE_PREFIX_PATH`/
   `LD_LIBRARY_PATH` 传入后续步骤；
2. **编译**：`cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DNEED_BENCHMARK=ON
   -DNEED_EXAMPLE=ON`（普通 PR 编译全部 examples；依赖外部服务的示例只编译不运行），
   固定 `JOBS=3`；
3. **自测**：`python3 scripts/test_release_gate.py` +
   `python3 -m unittest discover -s scripts/ci -p 'test_*.py'`；
4. **ctest**：`ctest --timeout 60 --output-on-failure`，最多 3 次有界重试（step 10min）；
5. **冒烟/可靠性**：`Test_smoke`（90s）/`Test_reliability`（120s）各自 3 次有界重试
   （step 各 8min）。

重试为有界 3 次，每次 attempt 独立进程、失败证据留在 job 日志；总时长由 step 级
`timeout-minutes` 界定。

## 5. 性能回归门禁（`性能回归检查`，required context）

**同 runner 自给自足的 base/head 比较**：同一 hosted job 内顺序 checkout
`perf-src-base`/`perf-src-head`（显式完整 40-hex SHA 校验，零 SHA 拒绝，不用 `HEAD^`
猜测），顺序构建两棵树的 `unified_stress`（**只构建该目标，不跑全量 CTest**），用
**head 的** `scripts/ci_perf_check.py` 分别测量六模块 45s×2threads：

- **base** 无基线测量（`--no-baseline-compare`）：verdict `NO_COMPARABLE_BASELINE`，
  exit 0——**只作 runner 临时基线，不是 PASS 声明**；任一模块测不干净
  （timeout/crash/zero ops/errors>0/缺字段）base 即 `exit 2`；
- **head** 以 base 报告为基线 + `--strict-comparable --gate-enabled` 严格比较：
  `PASS`/`WARN` → exit 0（WARN 走 `::warning::` 注解，不伪装 PASS）；
  `FAIL`/`METRIC_INVALID` → exit 2；**任一模块 `NO_COMPARABLE_BASELINE`** → exit 2；
  缺基线、缺模块、环境指纹不一致、`errors>0`、zero ops、NaN/inf 一律 `exit 2`——
  **缺证据不得置绿**。

**判定阈值（ADR D6）**：

| 模块 | WARN | FAIL（--gate-enabled） |
|---|---|---|
| 普通模块 | 退化 ≥10% | 退化 ≥20% |
| cocond | 退化 ≥30% | 退化 ≥40% |

CoCond 放宽原因：其 ops 由 frame/timeout 定时器节拍驱动，对调度抖动和 runner 时钟噪声
天然敏感，10% 会大量误报。

**信任边界**：job `permissions: contents: read`；顶层 `permissions: {}`；无
secret/OIDC/写权限/`pull_request_target`/`workflow_dispatch`/self-hosted；远程 `uses`
完整 40-hex SHA pin；`persist-credentials: false`；只 `pull_request`/`push` 事件；基线
仅 runner 临时——**不**读/写 `tests/baselines/`、**不**动 `perf-baseline` 分支、
**不**改仓库变量、**不**触碰 `release_gate.py`；该 job 的 result 进汇聚（见 §3），
perf 触发时 fail-closed，perf=false/dispatch 的预期跳过由 `expected_skips` 显式放行。

**设计边界**：同 runner 短测只覆盖 base↔head 同机相对退化，**不是** server3 长期
基线；跨机/长期趋势由 `record_baseline.py` + `perf-baseline` 分支承担（发布 Gate 写入）。

**subject/base 绑定**：PR = `head.sha`/`base.sha`（真实 head，不冒充 merge commit）；
push main = `github.sha`/`event.before`。报告用 `--commit-sha` 绑定各自源码完整 SHA。

**故障分类**：runner/环境故障看 `编译 & 单元测试` 是否同挂与报告 reason 键；harness
故障 = `METRIC_INVALID`（指标缺失/进程崩溃）；代码性能回退 = 指纹一致下的 WARN/FAIL
delta。三者不混算。

## 6. 内存检测（memcheck.yml）

独立于 ci.yml：每周五 17:30（UTC+8）定时 + `workflow_dispatch`。单 job
（build + memcheck）共享同一工作区，跑在专用 `arc-s4-memcheck`（镜像须为
`bbtools-runner:v2-memcheck`，含 valgrind/libc6-dbg），`timeout-minutes: 120`。

步骤：

0. `test "$(cat /etc/bbttools-runner/profile)" = "bbtools-runner:v2-memcheck"` —
   校验专用 runner 镜像来源；不匹配即 FAIL；
1. `python3 scripts/ci/run_memcheck.py --check-env` — 环境前置校验（fail-closed；
   缺 valgrind/loader 符号/Boost 1.90 直接失败，不临时 apt 修绿）；
2. `scripts/ci/build_memcheck_target.sh` — 只构建 `mem_check_test`
   （`NEED_VALGRIND=ON`，RelWithDebInfo）；
3. `python3 scripts/ci/run_memcheck.py --binary … --report-dir tests/ci-reports/memcheck
   --timeout 1800` — 运行并判定；
4. 上传 `memcheck-report` artifact（日志 + summary.json/md，`if: always()`）。

前置校验与判定步骤各自先执行 `ulimit -n 65536`（Valgrind 3.22 在 Docker 默认超大
`RLIMIT_NOFILE` 下无法启动），且 `set -o pipefail` 防 `| tee` 吞退出码。

**判定口径（fail-closed，不用抑制文件）**：缺工具、缺/空日志、`Fatal`、崩溃（信号）、
超时、无 `ERROR SUMMARY`、`definitely lost != 0`、`indirectly lost != 0`、缺完成标记、
完成标记重复/冲突、必需计数缺失或不符，一律 FAIL 且退出码非 0。完成标记必须是唯一
一条 `BBT_MEMCHECK_DONE`，七个必需计数固定期望（与 `mem_check_test.cc` 打印一致）：

```text
inner=10000/10000  lock=4000/4000  chan_read=500000/500000
writer_done=50/50  failures=0/0    runtime_drained=1/1
stack_pool_drained=1/1
```

`possibly lost`/`still reachable` 原样记录但不作门禁。**不得**用 suppression 把
`definitely/indirectly lost` 静默过滤修绿。

**NEED_VALGRIND（协程栈登记）**：`NEED_VALGRIND=ON` 时 `Stack` 构造把可用栈区间登记
给 Valgrind（消除 boost.context 切换误报）；默认生产构建 OFF，找不到
`valgrind/valgrind.h` 时 CMake `FATAL_ERROR`。登记只消除 stack-switch 警告，**不等同**
泄漏判定已修复；历史观测的 lost 计数未经同负载对照前不得认定为全误报。

## 7. 发布 Gate（release.yml）

唯一发布入口，`workflow_dispatch` + Environment 审核；`gate` job 跑在 `arc-s4`，
`timeout-minutes: 1500`（默认 1h 压测，可经 `soak_seconds` 提到 24h）。完整流程：

1. `scripts/test_release_gate.py` 发布工具自测；
2. `scripts/release_gate.py validate`：版本格式/tag 尚不存在/source_sha 为当前 main
   HEAD、该 SHA 的 `ci.yml` main push run 汇聚 `result` job success，且 `编译 & 单元测试`
   与 `性能回归检查` 两个 context 均为 success 或经验证的预期 skipped（预期跳过
   只发生在 docs-only/perf=false 的 main push，由 fail-closed 的 result job 认证；
   缺失/failure/cancelled 一律拒绝）；
3. perf-baseline 分支可达性预检（缺失提前失败，不跳过 Gate）；
4. Release 构建（`-DNEED_TEST=ON -DNEED_BENCHMARK=ON -DNEED_EXAMPLE=ON`）；
5. 全量 CTest（step 15min，`ctest --timeout 60`）；
6. **真实客户端验收**：`scripts/acceptance_real_clients.py` 驱动 `echo_server` 与
   `co_with_hiredis`（Redis 不可用必须失败，不走 skip）；
7. **性能发布 Gate**：`ci_perf_check.py --gate-enabled` → 报告六模块齐全、verdict ∈
   {PASS, WARN}、每模块 status ∈ {PASS, WARN} 且 `errors==0`、commit 绑定 source_sha；
8. 并行疲劳压测：`run_parallel_stress.sh "$SOAK_SECONDS" 2`（六模块独立进程，
   计划时长后 TERM 120s + 30s 兜底，必须产出非空 summary.txt）；
9. Gate 通过后 `record_baseline.py record` 记录正式基线（任一模块
   no_data/超时/指标无效拒绝落盘）+ `trend` 趋势检查（连续小幅退化仅告警，须建专项
   Issue）+ 推送 `perf-baseline` 分支（每机器目录裁剪至最近 20 份）；
10. `publish` job（独立 runner + `release-rc`/`release-stable` Environment 审核）经
    `release_gate.py publish` 创建不可移动 tag 与 Release；任一 Gate 失败不创建
    tag/Release，已创建 tag 不移动不删除。

`gate` job `permissions: contents: write`（仅用于推送 `perf-baseline` 基线）；发布
凭据（`v*` tag deploy key，secret `RELEASE_TAG_SSH_KEY`）只在 publish job 使用。

## 8. 并发、安全与 fork 边界

- 普通 CI concurrency：push/main group 含 `run_id` → 逐提交独立、互不取消（保住每个
  SHA 的 main 结果供 release_gate 按 head_sha 读取）；PR group 用 `github.ref` → 只
  取消同一 PR 的旧 run。release group `release` 不取消。docs-check 取消同 PR 旧 run。
- 全部普通 job 固定 hosted `ubuntu-24.04`（不用浮动 `ubuntu-latest`）；ARC 只留给
  perf（`arc-s4` 发布 gate）与 memcheck（`arc-s4-memcheck`）。
- 远程 `uses` 全部完整 40-hex SHA pin；`persist-credentials: false`；顶层
  `permissions: {}`（ci）/最小读权限（memcheck、release）。
- 仓库公开：fork PR 执行的是 PR head 里的 workflow 文件，YAML `if`/paths 挡不住被
  替换的 workflow。硬闸门是 Actions `approval_policy=all_external_contributors`
  （外部贡献者的 fork workflow 默认不跑，须人工批准）；该策略不构成准入或隔离保证
  （有写权限者批准后仍会执行未审查的 workflow）。受支持的贡献路径是同仓分支 PR。
- 顶层 `permissions` 收紧、无 `secrets: inherit`/OIDC/`id-token`；`result`（`CI/result`）
  是唯一汇聚 check（fail-closed），远端 required 集合以 ruleset 实时回读为准
  （2026-10-30 快照仍为 `编译 & 单元测试` + `性能回归检查` 两个 context；docs-only/
  perf=false 产生的预期 skipped 由 GitHub 视为满足 required）。

## 9. 本地复现与离线验证

```bash
# 构建（Release，与 CI 同参数；新 configure 加 ccache，JOBS 上限 nproc/4、最小 2）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNEED_TEST=ON -DNEED_BENCHMARK=ON -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
JOBS=$(( $(nproc) / 4 )); JOBS=$(( JOBS < 2 ? 2 : JOBS ))
cmake --build build --parallel "$JOBS"
ctest --test-dir build --output-on-failure
./build/bin/tests/Test_smoke --log_level=test_suite

# CI 脚本与契约自测（仅标准库；结构契约需 PyYAML，缺失显式 SKIP）
python3 scripts/test_release_gate.py
python3 -m unittest discover -s scripts/ci -p 'test_*.py'

# docs-check 本仓自检
python3 -I scripts/ci/check_docs.py --root . --readme-max 200

# format-diff（需 clang-format-diff 或 git-clang-format）
bash scripts/ci/check_format_diff.sh origin/main

# 性能冒烟（不比较基线）
python3 scripts/ci_perf_check.py --module=comutex --threads=2 --dur=15 --no-baseline-compare

# 本地两阶段对比（等价 CI base/head 语义）
python3 scripts/ci_perf_check.py --build-dir <base-build> --commit-sha <base-sha> \
  --no-baseline-compare --dur 45 --threads 2 --output /tmp/perf-base.json
python3 scripts/ci_perf_check.py --build-dir <head-build> --commit-sha <head-sha> \
  --baseline /tmp/perf-base.json --strict-comparable --gate-enabled \
  --dur 45 --threads 2 --output /tmp/perf-head.json

# memcheck（需本机有 valgrind 与 valgrind/valgrind.h）
ulimit -n 65536
python3 scripts/ci/run_memcheck.py --check-env
BBT_VALGRIND_INCLUDE=/path/to/valgrind-include \
  bash scripts/ci/build_memcheck_target.sh "$PWD"
python3 scripts/ci/run_memcheck.py \
  --binary build-memcheck/bin/benchmarks/mem_check_test \
  --report-dir tests/ci-reports/memcheck --timeout 1800

# 基线/趋势（正式语义仅发布 Gate 用；本地试验输出到别处）
python3 scripts/record_baseline.py record --threads=2 --dur=60 --quick
python3 scripts/record_baseline.py trend
```

压测与冒烟 harness：`python3 scripts/ci/run_smoke.py --build-dir build-ci-smoke`、
`python3 scripts/ci/run_soak.py --build-dir build-soak`（先构建 `unified_stress`）；
并行压测 `INTERVAL=60 bash scripts/run_parallel_stress.sh 3600 2`。报告落
`tests/reports/`（不入仓）；`run_parallel_stress.sh` 汇总要求六模块 `ops_total>0`、
`errors=0`、达计划时长，任一无效判 FAIL。

## 10. 参考

| 文档 | 说明 |
|---|---|
| `.github/workflows/ci.yml` | 普通 CI workflow 权威定义（唯一汇聚 check：`CI/result`；远端 required 以 ruleset 回读为准） |
| `.github/workflows/release.yml` | 发布 Gate 权威定义 |
| `.github/workflows/memcheck.yml` | 内存检测权威定义 |
| `.github/workflows/ci.yml 的 docs-check job` | 文档快速反馈权威定义 |
| `docs/guides/development.md` | 开发、验证阶梯与发布流程真源 |
| `docs/guides/performance.md` | 性能分析与测量协议 |
| `docs/guides/real-client-acceptance.md` | 真实客户端验收场景 |
| `docs/conventions.md` | §10 CI 骨架约定 |
| `scripts/run_parallel_stress.sh` | 六模块并行压测 |
| `scripts/run_fatigue.py` | 串行疲劳测试运行器 |
