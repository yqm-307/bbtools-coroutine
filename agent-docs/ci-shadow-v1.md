# ci-shadow-v1 —— bbtools-coroutine hosted-only 普通 CI 影子门禁（Issue #50）

**状态：** 本地候选（未 commit / 未 push / 未建 PR / 未触发 CI）。远端写入与 PR 由父级执行。
**范围：** 仅新增文件，不改动任何现役文件（`.github/workflows/unit_test.yml`、`release.yml`、
`docs-check.yml`、`memery_test_info.yml`、产品 C++、`CMakeLists.txt`、依赖锁、docker、
required 规则、runner 标签、`scripts/release_gate.py`、现役 perf 基线路径、memcheck）。

## 1. 这个影子做什么

影子门禁 `ci-shadow-v1` 在一个**完全独立身份**的 workflow 里，复用现役 Layer 1 的
构建与测试判据，并复用**已发布并固定 SHA** 的 framework 分类/结果 reusable callee，
在不触碰现役门禁配置的前提下，验证「同一套判据能否在 hosted runner 上稳定复现」。

- 触发面最小：仅 `push` 到分支 `ci/issue-50-hosted-shadow`，以及指向 `main` 的 PR。
  无 `push[main]`、无 `schedule`、无 `workflow_dispatch`、无 `merge_group`。
- 身份独立：workflow 名 `ci-shadow-v1`、各 job/check 名均以 `shadow` 前缀命名、
  concurrency group 前缀 `ci-shadow-v1-`，与现役 `CI-${{ github.ref }}` 不冲突。
- 不新增 required check；影子失败不改变现役权威结果。

## 2. 交付物（仅新增）

| 文件 | 职责 |
|---|---|
| `.github/workflows/ci-shadow-v1.yml` | 影子 caller：变更集 → 复用已发布 callee 分类 → 等价构建/测试 → 复用 callee 汇聚结果 |
| `scripts/ci/changed_files.py` | 真实 git diff → 受限路径列表（通用 recipe，来源见下） |
| `scripts/ci/prepare_boost.sh` | 源码复现锁定 Boost 1.90 `context` 到工作区私有前缀（hosted 工具链缺口） |
| `scripts/ci/test_shadow_ci.py` | 离线冒烟 + 直接耦合测试（`test_*.py`，被现役 unittest discover 自动发现） |

通用 recipe 来源与局限（逐字复制、记录来源）：

- `changed_files.py` 复制自已独立审查的 bbtools-infra #50 影子候选
  `infra-worktree/scripts/ci/changed_files.py`
  （sha256 `bbe31710e256b9b9464fdbb363a677974b110a4046a3dc1bef3ccd2d9cf9e70c`）。
  它只依赖 git，与仓库/语言无关；**不是**分类/结果公共契约，本仓不复制该契约。
- `prepare_boost.sh` 复制自 `infra-worktree/scripts/ci/prepare_boost.sh`
  （sha256 `26f44c18331539a4bdbb830aae739842fc7b25a4d625c356e3de6d5a6ac5f9e0`）。
  只编 CMake 实际消费的 `context` 组件；上游若新增 Boost 编译组件需在此追加。

## 3. 判据等价（与现役 unit_test.yml 的 Layer 1）

影子 `build` job 逐项对齐现役「编译 & 单元测试」job，并由 `test_shadow_ci.py` 的
`UnitTestYmlCouplingTests` 直接读两个真实 workflow 文件比对，防漂移：

| 判据 | 现役 unit_test.yml | 影子 |
|---|---|---|
| 构建 | `cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DNEED_BENCHMARK=ON` + `cmake --build . --parallel "$JOBS"`（`JOBS: 3`） | 同左 |
| 工具自测 | `python3 scripts/test_release_gate.py` + `python3 -m unittest discover -s scripts/ci -p 'test_*.py'` | 同左 |
| 全量单测 | `ctest --timeout 60 --output-on-failure`（串行），最多 3 次有界重试 | 同左 |
| 冒烟 | `timeout --signal=TERM --kill-after=10s 90s bin/unit_test/Test_smoke --log_level=test_suite`，3 次 | 同左 |
| 可靠性 | `timeout --signal=TERM --kill-after=10s 120s bin/unit_test/Test_reliability --log_level=test_suite`，3 次 | 同左 |

重试为 **3 次有界**（不多不少），每次 attempt 独立进程、独立日志，失败证据留在 job 日志；
总时长由 step 级 `timeout-minutes` 界定。**不写成 first-pass，也不扩重试/放宽检查。**

影子**不含**：性能比较、基线读写、发布、memcheck（由 `CouplingTests.test_shadow_excludes_perf_release_memcheck`
断言）。PR 自身触发现役完整 CI（含 `性能回归检查`）仍由父级授权的普通合入门禁承担，影子不替换它。

## 4. hosted-only 与工具链

- 全部本地 job 固定 `runs-on: ubuntu-24.04`；顶层 `permissions: {}`，仅需 checkout 的 job
  授予 `contents: read`。无 secret / OIDC / 写权限 / secrets 透传；`uses` 全部完整 40-hex SHA pin
  （`actions/checkout` `11d5960a326750d5838078e36cf38b85af677262`，经 `git ls-remote` 只读核验）。
- 无 artifact（不产出/不消费/不下载）、无 cache（每次干净全量构建，等价现役 cache-bypass 路径）。
- 工具链缺口只有 Boost 1.90：hosted runner 预装的是系统 Boost（版本不同）。`prepare_boost.sh`
  按 framework 已审查发布的 `docker/toolchain.lock`（`BOOST_VERSION=1.90.0` /
  `BOOST_SHA256=5e93d582…`）下载并 sha256 fail-closed，**源码编译** `context` 到工作区私有前缀
  （不写 `/usr/local`、不装系统包、不改镜像），经 `BOOST_ROOT`/`CMAKE_PREFIX_PATH`/
  `LD_LIBRARY_PATH` 传入构建与测试步骤。
- hosted 实际 `cmake` / `g++` / `ninja` 版本由 job 内「采集 runner 工具链版本」步骤读回作证据
  （现役 ARC 实测为 g++13.3 / cmake3.28.3 / Boost1.90(context)）；本仓 CMake 仅需 `>=3.16`。

## 5. 分类与结果：真实复用已发布 callee

- caller 只调用 `yqm-307/bbt-framework/.github/workflows/bbtools-classify-v1.yml@1c0b0fb0ebc8e7ca3888aa4f6a74d1baecf08cc7`
  （只读 GET 返回 200，242 行，sha256 `56f5a0af7acd73352180564ae9c3dd67d770b65fc20ceaa07f0bdec44ad4a578`）。
  `plan` job 做 classification-only（不传 `results_json`）；`result` job（`if: always()`）把三个
  真实 `needs.*.result`（含 skipped）交回 callee 的 result 契约。
- `required_checks_json=["changes","plan"]`、`optional_checks_json=["build"]`：互斥、非空
  （由 callee 校验）。`build` 仅当分类为 code/unknown 时运行；docs-only 允许显式跳过。
  **classifier 失败 → unknown ≠ docs-only → 禁止跳过 `build`**，不借 docs 变绿。
- 变更集来自本仓真实 diff（`changed_files.py`）：PR 三点 diff、push `before..sha`（首推回退
  `HEAD^..HEAD`）；未知 revision / diff 失败 / 超预算（>3000 文件或 >60KB）一律保守回退空列表
  → callee 判 unknown → 执行全部。**不接受调用方任意 shell/jobs 输入**，本仓不复制第二套分类契约。
- callee 只做分类/结果，**不承担 C++ 构建**；构建在本仓 `build` job 内完成。

## 6. concurrency

```
group: ci-shadow-v1-${{ github.event_name == 'pull_request' && github.ref || format('run-{0}', github.run_id) }}
cancel-in-progress: ${{ github.event_name == 'pull_request' }}
```

- push：group 含 `run_id` → 每次提交独立、互不取消（保逐提交结果）。
- PR：group 用 `github.ref`（`refs/pull/<N>/merge`）→ 只取消同一 PR 的旧 run。
- 与现役（`CI-${{ github.ref }}`）不冲突。

## 7. 本地验证（真实结果）

```bash
cd <coroutine-worktree>
export PYTHONDONTWRITEBYTECODE=1

# (1) 生产等价（默认 python3 无 PyYAML、无 BBT_CI_SHARED_DIR）——全量发现
TMPDIR=/tmp python3 -m unittest discover -s scripts/ci -p 'test_*.py'
#   -> Ran 202 tests ... OK (skipped=16)
#      新增 48：配方守卫 8 + changed_files 路由 4 + workflow 文本契约 14 + 与 unit_test.yml 耦合 6
#                +（本环境）结构契约 7 / 分类路由 9 显式 SKIP（SKIP 不等于通过）

# (2) 全绿本地：PyYAML 6.0.3 + 已发布 callee 逻辑（真实复用，不复制契约）
BBT_CI_SHARED_DIR=<framework>/scripts/ci/shared TMPDIR=/tmp \
  /usr/bin/python3 -m unittest discover -s scripts/ci -p 'test_*.py'
#   -> Ran 202 tests ... OK（我的 48 项全绿；分类路由直接调用真实 cli.py classify/evaluate）
```

父级在自有 scratch 复跑完整 202 项时，既有 `test_accepts_build_outside_home_and_source`
因 TMPDIR 位于真实 HOME 内而拒绝目录；未改测试或安全校验。仅对子进程设置 mock HOME 后，
PyYAML + shared 环境 202 项通过（0 skip），默认环境 202 项通过（16 项显式 skip）。
真实 HOME 环境的失败日志仍保留，mock HOME 对照不等同于该环境原样通过。

分类路由冒烟覆盖（用**同一个真实 callee CLI**）：docs-only / code / classifier 失败=unknown /
空变更=unknown；`docs-only + build skipped → success`；`code + build failure → failure`；
`required skipped → failure`；`unknown + build skipped → failure`；`required cancelled → failure`；
`code + build success → success`。

只读远端核验（未触发 CI）：已发布 callee 文件 HTTP 200；`actions/checkout v4` SHA 经
`git ls-remote` 命中；framework `docker/toolchain.lock` 的 Boost 版本/sha256 与该 SHA 下内容一致。

## 8. 未覆盖（本地不冒充已验）

- 任何**真实 hosted run**（reusable 跨仓在线身份/权限、hosted 工具链实际版本、Boost 下载校验的
  真实执行）——走父级授权的 PR CI。
- 任何 **C++ 全量构建 / ctest 真实执行**（本地只做 bash -n / 静态契约 / 配方守卫 / 真实 cli 路由）。
- 机器采购、self-hosted 准入、生产发布链路——本轮不触及（无准入证据，保持不动）。

## 9. 非等价 / 需注意（未阻塞）

1. 已发布 callee 内部纯逻辑 job 固定 `ubuntu-latest`（caller 不可改；当前 `==24.04`）。
2. hosted `cmake` 版本可能高于现役 ARC 的 `3.28.3`（本仓只需 `>=3.16`）；真实 run 需读回实际版本。
3. Boost 只编 `context`（现役 CI 日志实证的唯一编译/链接组件）。
4. 影子构建步骤与现役 `unit_test.yml` 各存一份（本轮不改 unit_test.yml）；已用 `UnitTestYmlCouplingTests`
   直接读两个真实 workflow 比对兜底。根治需现役 unit_test.yml 改调同一脚本，本轮未授权。
5. 影子对「非 docs 变更的 `build` 被意外跳过」比现役更严（required 失败/取消/意外 skipped 均不得绿）。
