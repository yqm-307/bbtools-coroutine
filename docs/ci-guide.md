# bbtools-coroutine CI 使用文档与团队接入指南

> 开发、main 集成和版本发布的唯一流程真源：
> [`agent-docs/development-and-release-process.md`](../agent-docs/development-and-release-process.md)。
> 普通 CI 的机制与配方细节：`agent-docs/formal-ci.md` 与 `agent-docs/perf-check-wiring.md`；
> 本文只给使用口径与本地命令，不再第三处复制 CI 内部实现。
>
> 本文档说明 bbtools-coroutine 的 CI 流水线：如何触发、如何解读结果、常见问题处理、
> 如何添加测试、如何本地验证。

---

## 1. 快速上手

### 我是贡献者，我要提 PR

```bash
# 1. 基于最新 main 开分支
git fetch origin && git checkout -b feat/my-change origin/main

# 2. 修改代码 + 本地聚焦验证（必做！）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DNEED_BENCHMARK=ON \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
JOBS=$(( $(nproc) / 4 )); JOBS=$(( JOBS < 2 ? 2 : JOBS ))
cmake --build build --parallel "$JOBS"
ctest --test-dir build --output-on-failure               # 全量仅按需；普通改动按验证阶梯做聚焦验证
./build/bin/unit_test/Test_smoke --log_level=test_suite  # 冒烟测试

# 3. 推送分支 → 创建 PR
git push -u origin feat/my-change
# GitHub 上创建 PR，CI 自动运行编译+测试
```

CI 在 PR 创建/更新时运行分类、编译+CTest、Smoke、Reliability 和同 runner 性能回归门禁（分钟级）。
**发布级检查（真实客户端验收、可配置时长的疲劳压测、基线写入）只在发布 Gate 执行**。

### 我是 reviewer，要审 PR

CI 通过（编译+ctest+smoke 全绿、性能回归未超阈值）是 merge 的前提条件。
检查 PR 页面上的 CI checks 状态：

| 信号 | 含义 | 动作 |
|------|------|------|
| ✅ `编译 & 单元测试` pass | 编译通过+全部测试通过 | 可以 review 代码 |
| ✅ `性能回归检查` pass | 同 runner base/head 45s/模块严格比较未超阈值 | 可以 review 性能敏感改动 |
| `编译 & 单元测试` skipped | 分类器判为 docs-only，重型 build job 显式跳过（不伪造 success） | 仅文档变更属正常 |
| ❌ required check fail | 分类、编译、单测或性能门禁失败 | 要求修复后重推 |

**性能影响：** `性能回归检查` 在 PR 和 push main 都执行（同 runner 顺序构建 base+head 的
`unified_stress`，六模块 45s×2 threads 严格比较；缺证据即失败）。完整性能 Gate、长时疲劳压测和
基线写入只在发布 Gate 执行。性能敏感变更仍应在合入前定向验证。

### 1.1 真实客户端验收（发布 Gate）

`真实客户端验收` 使用 `scripts/acceptance_real_clients.py`，由 CI 独立构建示例目标并直接运行：

- Echo：阻塞式 POSIX socket、多客户端、随机二进制 payload、碎片发送、半关闭、异常断连和重连；
- hiredis：Redis 长连接复用、随机变长值、`SET` / `GET` / `EXISTS` / `DEL` 和错误统计；
- Redis 不可用属于 CI 环境失败，不得转成发布通过。

本地命令：

```bash
python3 scripts/acceptance_real_clients.py \
  --echo-server ./build/bin/example/echo_server \
  --hiredis ./build/bin/example/co_with_hiredis
```

### 我是 CI 维护者，管道挂了

见 [§7 FAQ](#7-faq) 的「流水线挂了怎么排查」。

---

## 2. CI 触发机制

### CI 工作流：unit_test.yml（普通门禁）与 docs-check.yml（文档快速反馈）

普通门禁 `.github/workflows/unit_test.yml` 对 `pull_request → main` 与 `push → main` 运行，
按 `scripts/ci/changed_files.py` 产出的真实 diff 交给 framework 分类 callee 分类：

| 分类 | `编译 & 单元测试`（build job） | `性能回归检查`（perf job） |
|------|------------------------------|----------------------------|
| code / unknown / 分类失败 | ✅ 全量构建 + CTest + Smoke + Reliability | ✅ 同 runner base/head 严格比较 |
| docs-only | 显式 skipped（optional 语义允许，不靠空成功伪造绿） | ✅ 照常运行 |

`docs-check.yml` 是文档类路径（`**.md`、`docs/**`、`agent-docs/**`、`LICENSE`、图片）上的
轻量快速反馈（<5min：空格 diff 检查 + 密钥扫描，fail-closed），与 unit_test.yml 不互斥。
混合变更（文档+代码）走 unit_test.yml 全量。

### 运行环境

普通 CI 全部 job 运行在 **hosted `ubuntu-24.04`**；不再使用常驻 self-hosted runner。
发布 Gate 的 `gate` job 仍在 `arc-s4`，内存检测在专用 `arc-s4-memcheck`（见 §4）。

仓库公开。fork PR 会执行 PR head 里的 workflow，不能靠本仓库 YAML 的 `if` 隔离 runner。硬闸门是
Actions `approval_policy=all_external_contributors`：外部贡献者的 fork workflow 默认不跑，必须人工
批准。不要批准未审查的 fork workflow。受支持的贡献路径是同仓分支 PR。

### 超时设置

| 阶段 | timeout | 说明 |
|------|---------|------|
| `变更类型检测` | 5 min | 真实 diff + 分类输入 |
| `编译 & 单元测试`（build job） | 75 min | 含 Boost 1.90 context 源码前缀准备、全量构建、CTest（step 10min）、Smoke / Reliability（step 各 8min） |
| `性能回归检查`（perf job） | 60 min | 两棵树顺序构建 `unified_stress` + 六模块 45s×2 测量 |
| `docs-check` | 5 min | 空格 + 密钥扫描 |
| release / gate | 1500 min | 含构建、验收、性能 Gate、可配置时长压测（默认 1h，最长 24h）与基线写入；不是实测完成时间承诺 |
| release / CTest | step 15 min | `ctest --timeout 60`，避免单测挂死吃掉整个 Gate 的时间预算（gate job timeout 见 workflow） |
| release / 真实客户端验收 | step 10 min | Redis 缺失必须失败，不走 skip |

---

## 3. 流水线详解

```
每个 PR/push main（分钟级）:
  变更类型检测（真实 diff）→ framework 分类 callee（plan）
    → 编译 & 单元测试（docs-only 显式 skip）
    → 结果汇聚（always，fail-closed）
  性能回归检查（独立 context）：同 runner 顺序构建 base+head 的 unified_stress
    → base 无基线测量（runner 临时基线）→ head --strict-comparable 严格比较

发布 Gate（workflow_dispatch + Environment 审核）:
  Release 构建 → 全量 CTest → 真实客户端验收 → 严格性能 Gate
  → 可配置时长并行疲劳压测（soak_seconds，默认 3600）
  → 性能基线记录 / 趋势检查 / 推送 perf-baseline
```

### 3.1 编译与单元测试（code/unknown 时必跑；docs-only 显式跳过）

**步骤（见 `.github/workflows/unit_test.yml`）：**

1. **工具链：** `scripts/ci/prepare_boost.sh` 按锁定 SHA256 源码构建 Boost 1.90 `context`
   到工作区私有前缀（hosted 预装 Boost 版本不同，不借用）；
2. **编译：** `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DNEED_BENCHMARK=ON`，
   固定 `JOBS=3`；**不存在** build artifact 打包/跨 job 复用——perf job 在同一 runner 上
   独立 checkout 两棵源码树并自行构建 `unified_stress`；
3. **自测：** `python3 scripts/test_release_gate.py` + `python3 -m unittest discover -s scripts/ci -p 'test_*.py'`；
4. **ctest：** `ctest --timeout 60 --output-on-failure`，最多 3 次有界重试（step 10min）；
   注册套件数以构建目录 `ctest -N` 输出为准，不硬编码数量；
5. **冒烟 / 可靠性：** `Test_smoke` / `Test_reliability` 各自有界重试（step 各 8min）。

本地完整测试清单以 `ctest -N` 为准；新增测试必须在 `unit_test/CMakeLists.txt` 用
`add_test()` 注册（见 §8）。`Test_scheduler_stop` 等已删除的套件不得再列为现行可执行测试目标；
历史报告、迁移警告与故障查阅中的旧名称及其版本边界按原文保留，不据此改写历史记录。

真实客户端验收不属于普通门禁；`NEED_EXAMPLE=ON` 且 `NEED_TEST=ON` 时本地额外注册
`Test_real_clients_echo`（无条件必须 PASS）与 `Test_real_clients_hiredis`（无 Redis 时 skip 77）。

**失败含义：**
- 编译失败 → 代码有语法/链接错误，检查编译日志
- ctest 失败 → 某测试用例失败，检查对应模块的 `--output-on-failure` 输出
- Test_smoke 失败 → 核心模块 happy-path 被破坏
- docs-only 之外 build skipped → 分类/汇聚 fail-closed，不是绿

### 3.2 并行疲劳压测（仅发布 Gate）

**条件：** 发布 Gate（`workflow_dispatch`）且 Environment 审核通过。

**运行方式：** `INTERVAL=60 bash scripts/run_parallel_stress.sh "$SOAK_SECONDS" 2`（`soak_seconds` 默认 3600，范围 60-86400）

6 个模块独立进程并行运行：
- `comutex, corwmutex, cocond, chan, copool, coroutine`
- 每模块 2 个 processer 线程，60s 间隔采样
- job 超时 1500min（覆盖最长 24h 压测）；模块进程在计划时长后 120s 发送 TERM，再给 30s 退出兜底

**产物：**
- 脚本默认把汇总报告写在 `tests/reports/<timestamp>/summary.txt`，模块日志 `tests/reports/<timestamp>/<module>.log`
- 发布 Gate 中输出目录为 `tests/reports/release-<run_id>-<run_attempt>`（workflow 的 `OUTDIR`），artifact 名称 `release-gate-<run_id>-<run_attempt>`，均以 `release.yml` 定义为准；本地运行时报告仍只写入 `tests/reports/`

### 3.3 性能回归门禁

普通门禁 `性能回归检查` 是**同 runner 自给自足**的 base/head 比较：

- 同一 hosted job 内顺序 checkout `perf-src-base` / `perf-src-head`（显式完整 SHA 校验），
  顺序构建两棵树的 `unified_stress`（不跑全量 CTest），用 **head 的**
  `scripts/ci_perf_check.py` 分别测量六模块 45s × 2 threads；
- **base** 无基线测量（verdict `NO_COMPARABLE_BASELINE`，exit 0）——只作 runner 临时基线，
  **不是** PASS 声明；任一模块测不干净 base 即 `exit 2`；
- **head** 以 base 报告为基线 + `--strict-comparable --gate-enabled` 严格比较：退化 ≥20%
  （CoCond ≥40%）FAIL；缺基线、缺模块、环境指纹不一致、`errors>0`、zero ops、NaN/inf 一律
  `exit 2`——**缺证据不得置绿**；
- 基线仅 runner 临时，**不**读/写 `perf-baseline` 分支、不改仓库变量、不触碰
  `release_gate.py`。该 job 不进 framework result 汇聚（独立 context）。
- 详细判定链、信任边界与退出码见 `agent-docs/perf-check-wiring.md`。

**判定阈值（沿用 ADR D6）：**
| 模块 | WARN | FAIL（--gate-enabled） |
|------|------|------|
| 普通模块 | 退化 ≥10% | 退化 ≥20% |
| cocond | 退化 ≥30% | 退化 ≥40% |

CoCond 放宽原因：其 ops 由 frame/timeout 定时器节拍驱动，对调度抖动和
runner 时钟噪声天然敏感，10% 会大量误报（历史压测观察）。

**Release 基线与趋势：** 发布 Gate 在长测通过后才 `record_baseline.py record` 生成正式基线并
推 `perf-baseline` 分支，`trend` 检查连续退化；这与普通门禁的 runner 临时基线是两条路径。
`record_baseline.py record` 在任一模块 no_data/超时/指标无效时拒绝落盘（exit 2）；失败、取消、
超时的 run 不会覆盖好基线。**连续两次发布基线或发布前确认性能回退，必须创建专项修复 Issue**；
单次异常先复测并记录环境。推送步骤自动裁剪每机器目录至最近 20 份；workflow 不自动打基线 tag，历史基线经 `perf-baseline` 分支的提交历史追溯（如需固定发布索引另行决策）。

**故障分类：** runner/环境故障看「编译 & 单元测试」是否同挂与报告里的 reason 键；harness 故障 =
`METRIC_INVALID`（指标缺失/进程崩溃）；代码性能回退 = 指纹一致下的 WARN/FAIL delta。三者不混算。

**本地复现：**

```bash
# 构建（Release，与 CI 同参数；新 configure 加 ccache，JOBS 上限 nproc/4、最小 2）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNEED_TEST=ON -DNEED_BENCHMARK=ON -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
JOBS=$(( $(nproc) / 4 )); JOBS=$(( JOBS < 2 ? 2 : JOBS ))
cmake --build build --parallel "$JOBS"

# 冒烟（不比较基线）
python3 scripts/ci_perf_check.py --module=comutex --threads=2 --dur=15 --no-baseline-compare

# 本地两阶段对比（等价于 CI 的 base/head 语义）：先对被测构建树做无基线测量作临时基线，
# 再对另一构建树严格比较；CI 用的是同 runner 两棵隔离源码/构建树
python3 scripts/ci_perf_check.py --build-dir <base-build> --commit-sha <base-sha> \
  --no-baseline-compare --dur 45 --threads 2 --output /tmp/perf-base.json
python3 scripts/ci_perf_check.py --build-dir <head-build> --commit-sha <head-sha> \
  --baseline /tmp/perf-base.json --strict-comparable --gate-enabled \
  --dur 45 --threads 2 --output /tmp/perf-head.json

# 记录基线（仅发布 Gate 语义；本地试验用 --output 写到别处）
python3 scripts/record_baseline.py record --threads=2 --dur=60 --quick

# 趋势检查
python3 scripts/record_baseline.py trend
```

门禁契约测试：`python3 -m unittest discover -s scripts/ci -p 'test_*.py'`。

---

## 4. 内存检测（memery_test_info.yml / #379）

独立于 unit_test.yml 的工作流，**每周五 17:30（UTC+8）自动运行**，也支持手动触发。
单 job（build + memcheck）共享同一工作区，产物关系显式，不再隐式依赖跨 job 工作区。
job 跑在专用 ARC runner **`arc-s4-memcheck`**，其镜像必须是带 `valgrind`、`libc6-dbg`
以及 `/etc/bbttools-runner/profile=bbtools-runner:v2-memcheck` 的 `bbtools-runner:v2`。
该 scale set 与普通 `arc-s4` 的隔离是持续条件，不是一次性部署状态：每次运行都使用隔离专用 runner，
步骤 0/1 的镜像 profile 校验与环境前置校验必须通过，不匹配即失败；涉及 runner/镜像的变更仍按
授权处理。job 设 `timeout-minutes: 120` 有界。

**步骤：**
0. `test "$(cat /etc/bbttools-runner/profile)" = "bbtools-runner:v2-memcheck"` — 校验专用 runner 镜像来源；不匹配即 FAIL
1. `python3 scripts/ci/run_memcheck.py --check-env` — 环境前置校验（fail-closed）
2. `scripts/ci/build_memcheck_target.sh` — 只构建 `mem_check_test`（`NEED_VALGRIND=ON`，RelWithDebInfo）
3. `python3 scripts/ci/run_memcheck.py --binary … --report-dir tests/ci-reports/memcheck` — 运行并判定
4. 上传 `memcheck-report`（日志 + summary.json/md，`if: always()`）

前置校验步骤与构建步骤都在 `run` 内、管道执行前显式 `set -o pipefail`：GitHub 默认 shell 是
`bash -e {0}`（**没有** `pipefail`），不加这行 `| tee` 会把失败退出码吞掉、门禁形同虚设。
Valgrind 3.22 在 Docker 默认超大 `RLIMIT_NOFILE`（实测约 1e9）下会在启动期报
`Private file creation failed`；环境前置校验和判定步骤各自先执行 `ulimit -n 65536`，只影响该
step 的子进程，不改 runner 或宿主机配置。判定入口只在**前置校验全绿**时才启动 workload，并在进入判定路径时先清理 `report-dir` 下
本轮的固定产物（`<log-name>.log/.stdout.log/.stderr.log`、`summary.json/md`）、**必写**
summary（前置失败也落 FAIL）——上一轮的 PASS 报告不会残留下来随 artifact 归档；
`--check-env` 不写报告。

旧入口 `shell/workflow/memery_test_info/{compile_testfile,do_valgrind_memcheck}.sh` 保留为转发
（旧版 `rm -rf build` + 不判读结果、不设退出码，任何泄漏都会“绿”，已废弃）。

**NEED_VALGRIND（协程栈登记）：** bbt 协程用 `memalign` 的自定义栈，Valgrind 默认不把它当成栈，
`boost.context` 每次切换 SP 都被记为 `client switching stacks?`。`NEED_VALGRIND=ON` 时
`Stack` 构造把**可用栈区间**（不含 `PROT_NONE` 保护页）登记给 Valgrind，`Clear`/析构注销、
`move` 转移登记 ID。**默认生产构建（OFF）不依赖 Valgrind**；`NEED_VALGRIND=ON` 但找不到
`valgrind/valgrind.h` 时 CMake 直接 `FATAL_ERROR`。头文件默认在标准 include 路径与
`CMAKE_INCLUDE_PATH` 里自动搜索；私有解包场景用 `-DBBT_VALGRIND_INCLUDE=<dir>` 指定
（该目录下必须真的存在 `valgrind/valgrind.h`，否则同样 `FATAL_ERROR`）。

**⚠️ 登记只消除 stack-switch 警告，不等同“泄漏判定已修复”。** 历史观测到的 164MB / 1.7MB
（`definitely/indirectly lost`，分别疑似来自：栈底保护页使 memcheck 把栈块判 lost、以及
`StackPool` 的 boost::lockfree 队列 tagged_ptr 不可追踪）**未经同负载对照前不得认定为全误报**；
必须用“同程序、同工作量、同 worker，仅切换 `NEED_VALGRIND`”的对照来区分工具识别误差、
合法 process-lifetime 存活与真实泄漏。

**判定口径（fail-closed，不用抑制文件）：** 缺工具、缺/空日志、`Fatal`、崩溃（信号）、超时、
无 `ERROR SUMMARY`、`definitely lost != 0`、`indirectly lost != 0`、缺完成标记、
完成标记重复键、出现多条互相冲突的完成标记、必需计数缺失、或任一计数与固定期望不符，
一律 FAIL 且退出码非 0。完成标记必须是**唯一一条** `BBT_MEMCHECK_DONE`，七个必需计数的
固定期望（与 `mem_check_test.cc` 打印一致，任何缩水都 FAIL）：

```
inner=10000/10000  lock=4000/4000  chan_read=500000/500000
writer_done=50/50  failures=0/0    runtime_drained=1/1
stack_pool_drained=1/1
```

`possibly lost` / `still reachable` 原样记录但不作门禁（分别来自存活 worker TLS 与
process-lifetime 单例）。**不得**用 suppression 把 `definitely/indirectly lost` 静默过滤来“修绿”。

**完成协议（业务完成 ≠ 运行时已释放）：** 所有 `CountDownLatch::Wait()` 只在主线程调用；
协程内只 `Down()` 且不得阻塞 `Wait`。业务计数达标后，再有界等待栈池账目
`g_bbt_stackpoll->GetCurCoNum() == 0`（`Context` 是唯一栈借出方，`~Context` 归还，
而 `~Context` 由 `Processer` FINAL delete `Coroutine` 触发），即**所有借出的栈已归还
（`~Context` 已进入 FINAL 释放路径）**。该计数与 `PROFILE` 无关，**不依赖 `PROFILE=OFF`
时会恒 0 的 Profiler 计数**。超时即视为未完成并 FAIL，不靠延长等待或缩减负载变绿。

确认 `runtime_drained=1` 后，主线程调用 `StackPool::ReleaseUnused()` 释放栈池中为复用保留的
闲置栈，再要求 `AllocSize()==0 && GetCurCoNum()==0`，并输出 `stack_pool_drained=1/1`。
这是检测前的缓存排空，不是 `Scheduler::Stop()`，不 join worker，也不改变生产运行时的
process-lifetime 语义；若池未能排空，工作负载直接 FAIL。

**#379 已采用 A：回调释放先于栈归还。** `Context::~Context` 函数体先清空
`m_onyield_callback`、`m_user_main`（两个 `std::function` 先释放 target），令它们最后持有的
捕获对象同步析构，**之后**才 `g_bbt_stackpoll->Release(m_stack)`；置空顺序与成员逆序析构
一致，不改变对象布局、不新增通知/join/keepalive。因此 `cur == 0`（`runtime_drained == 1`）蕴含“这两个
callable 及其最后持有的业务对象已析构”，这是本计数能证明的**精确上界**。回归入口：
`unit_test/Test_coroutine_stack.cc` 的 `t_context_releases_callables_before_stack_return`
直接构造 `Context`，用析构探针断言捕获对象在栈归还前析构（只覆盖 `m_user_main` 路径）。

它**仍不**证明外部其它 owner 持有的资源已释放、worker 侧异步物理清理完成、
`delete Coroutine` 已返回或对象本体 deallocation——`delete` 发生在 worker 线程，main 线程
看到 `cur == 0` 即返回，两者没有 happens-before。该计数**不是**、也不得升级为通用
join/drain API（运行时是进程寿命对象，见
[核心运行时契约 §6](../agent-docs/2026-09-07-core-runtime-contract.md)）。

**跨仓镜像依赖（已完成一次受控运行态验证）：** 本仓脚本对 valgrind（含 `libc6-dbg`/loader 符号）、
Boost 1.90（runner 上位于 `/opt/boost/include`）和 runner 前置条件 fail-closed，禁止在 job 内
临时 `apt` 安装修绿。已在 `bbtools-runner:v2` + `arc-s4-memcheck` 等价运行环境完成一次
受控构建、单测和真实 Valgrind 验证；后续 runner 变更仍需保持与 `bbtools-runner:v1`、
普通 `arc-s4` 及 #378 隔离。正式 workflow 结果以对应 Actions run 和 artifact 为准。

**本地复现：**

```bash
# 前置 + 构建 + 判定（需本机有 valgrind 与 valgrind/valgrind.h）
ulimit -n 65536
python3 scripts/ci/run_memcheck.py --check-env
BBT_VALGRIND_INCLUDE=/path/to/valgrind-include \
  bash scripts/ci/build_memcheck_target.sh "$PWD"
python3 scripts/ci/run_memcheck.py \
  --binary build-memcheck/bin/benchmark_test/mem_check_test \
  --report-dir tests/ci-reports/memcheck --timeout 1800

# 判定逻辑契约测试（用真实退出码驱动反例，不依赖本机 valgrind）
python3 scripts/ci/test_memcheck_contract.py
```


---

## 5. 解读 CI 结果

### 5.1 GitHub Checks

PR 的必需检查为 `编译 & 单元测试` 与 `性能回归检查`；发布级 job（真实客户端验收、疲劳压测）只在发布 Gate 执行：

- ✅ **`编译 & 单元测试`** — 绿色 = 编译、CTest、Smoke、Reliability 全部通过；docs-only 分类下该 check 为 skipped（显式跳过，属正常）
- ✅ **`性能回归检查`** — 绿色 = 同 runner base/head 六模块 45s×2 严格比较未超阈值
- 发布 Gate 才检查 **`真实客户端验收`** 与并行疲劳压测
- ❌ 红色 = 失败，点击展开查看具体失败的 step 日志

### 5.2 ctest 输出解读

ctest 失败时展开 CI 日志中的 "ctest" step，查看具体失败的 test suite 和断言消息。

示例输出（套件数仅示意，实际以 `ctest -N` 为准）：
```
Test project /path/to/build
    Start 1: Test_coroutine_stack
1/N Test #1: Test_coroutine_stack .............   Passed    0.05 sec
    Start 2: Test_coroutine
2/N Test #2: Test_coroutine ..................   Passed    0.11 sec
...
100% tests passed, 0 tests failed out of N
```

### 5.3 压测结果解读

压测完成后 `run_parallel_stress.sh` 输出汇总表格：

```
     module          ops   errors  status
---------------------------------------------
    comutex   12,345,678        0  OK
  corwmutex   10,234,567        0  OK
     cocond    2,456,789        0  OK
       chan    5,678,901        0  OK
     copool      345,678        0  OK
  coroutine    1,234,567        0  OK
---------------------------------------------
      TOTAL   32,296,180        0
     result         PASS
```

**判断标准：**
- 全部进程成功退出，六模块 `ops_total > 0`、`errors=0`、`elapsed_s` 达到计划时长，才可判 PASS。
- 零操作、错误计数、缺失指标或提前结束均判 FAIL；错误原因需结合模块日志定位，不能仅凭计数断言是 CoCond 问题。

**性能回归：** CI 通过 `性能回归检查`（PR + push main）和发布 Gate 自动执行比较。`PASS` 正常，`WARN` 会显式标注但不阻塞，`FAIL`、`METRIC_INVALID` 与缺证据（strict 模式下含 `NO_COMPARABLE_BASELINE`）按 fail-closed 处理；不得把 `WARN` 说成 `PASS`。

---

## 6. 本地运行测试

### 6.1 编译

```bash
# 构建（新 configure 加 ccache；JOBS 上限 nproc/4、最小 2）
cmake -S /path/to/bbtools-coroutine -B /path/to/bbtools-coroutine/build \
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DNEED_BENCHMARK=ON \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
JOBS=$(( $(nproc) / 4 )); JOBS=$(( JOBS < 2 ? 2 : JOBS ))
cmake --build /path/to/bbtools-coroutine/build --parallel "$JOBS"
cd /path/to/bbtools-coroutine/build
```

### 6.2 单元测试

```bash
# 全部注册的单元测试（套件数以 `ctest -N` 为准）
ctest --output-on-failure

# 单个模块
./bin/unit_test/Test_comutex
./bin/unit_test/Test_chan --log_level=test_suite
./bin/unit_test/Test_cond
./bin/unit_test/Test_co_rwmutex
./bin/unit_test/Test_copool
./bin/unit_test/Test_hook
./bin/unit_test/Test_coevent
./bin/unit_test/Test_coroutine
./bin/unit_test/Test_coroutine_exception
./bin/unit_test/Test_coroutine_stack
./bin/unit_test/Test_poller
./bin/unit_test/Test_g_co
./bin/unit_test/Test_defer
./bin/unit_test/Test_lockguard
./bin/unit_test/Test_rwlock_guard

# 冒烟测试
./bin/unit_test/Test_smoke --log_level=test_suite
```

### 6.3 基准测试（benchmark）

```bash
# 100 万协程吞吐
./bin/benchmark_test/benchmark_coroutine
./bin/benchmark_test/benchmark_copool
./bin/benchmark_test/benchmark_chan
./bin/benchmark_test/benchmark_comutex
./bin/benchmark_test/benchmark_cocond
```

### 6.4 疲劳压测

```bash
# 快速验证：10s 间隔，30s 全模块
FATIGUE_INTERVAL=10 timeout -s KILL 45 ./build/bin/benchmark_test/unified_stress 30 0 0

# 模块隔离并行（推荐，等同 CI）：
INTERVAL=10 bash scripts/run_parallel_stress.sh 60 2   # 1 分钟快速
INTERVAL=60 bash scripts/run_parallel_stress.sh 3600 2  # 1 小时全量

# 单模块长时间压测
FATIGUE_INTERVAL=60 ./build/bin/benchmark_test/unified_stress --module=comutex --threads=4 600 0 0
```

### 6.5 Sanitizer 本地诊断

ASAN+UBSAN 用于检测 use-after-free、buffer overflow 等：

```bash
# 构建
mkdir build_san && cd build_san
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug -DNEED_BENCHMARK=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
JOBS=$(( $(nproc) / 4 )); JOBS=$(( JOBS < 2 ? 2 : JOBS ))
cmake --build . --parallel "$JOBS"

# 运行（约 2× 性能开销）
ASAN_OPTIONS=detect_leaks=0:halt_on_error=0 \
  ./bin/benchmark_test/unified_stress --module=comutex --threads=4 600 0 0
```

### 6.6 内存泄漏检测（RSS 监控）

Valgrind 在协程场景下不可靠，推荐 RSS 时序监控：

```bash
# 1h RSS 监控脚本
python3 -c "
import subprocess, time, os
p = subprocess.Popen(['./bin/benchmark_test/unified_stress', '3600', '0', '0'],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
prev_rss = 0
for i in range(60):
    time.sleep(60)
    with open(f'/proc/{p.pid}/status') as f:
        for line in f:
            if line.startswith('VmRSS:'):
                rss = int(line.split()[1])
                delta = rss - prev_rss
                tag = 'WARN' if delta > 1024 else 'OK'
                print(f'{tag} RSS @{i+1}min: {rss} kB (+{delta} kB)')
                prev_rss = rss
p.terminate()
"
```

判断标准：RSS 增长 < 100 MB/h 且趋势平稳 → 无泄漏。

---

## 7. FAQ

### Q: PR 的 CI required check 挂了怎么办？

展开 CI 日志定位是哪个阶段失败：

- **编译失败** → 检查本地是否用相同的 Ninja 构建通过。CI 用 Ninja + 私有 Boost 前缀编译，确认本地 configure 参数一致。
- **ctest 失败** → 展开 CI 日志看具体哪个 test case 失败，本地 `ctest --output-on-failure` 复现。
- **Test_smoke 失败** → 本地运行 `./bin/unit_test/Test_smoke --log_level=test_suite` 看完整输出。

确认本地也失败后修复，确保本地通过后再推。

### Q: 发布 Gate 的疲劳压测失败了怎么办？

检查 `tests/reports/` 下的压测日志：

1. **某模块 ops = 0** → 该模块冻结，检查是代码改动引入还是 runner 环境问题
2. **进程 crash / segfault** → 用 core dump 分析：`gdb build/bin/benchmark_test/unified_stress /tmp/core.*`
3. **进程被 OOM kill** → runner 内存不足，减少并发模块数或线程数

### Q: 如何本地复现 CI 的测试环境？

CI 的编译脚本等价于：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DNEED_BENCHMARK=ON
JOBS=$(( $(nproc) / 4 )); JOBS=$(( JOBS < 2 ? 2 : JOBS ))
cmake --build build --parallel "$JOBS"
cd build
ctest --output-on-failure
./bin/unit_test/Test_smoke --log_level=test_suite
```

本地跑一遍等价流程即可复现 CI 环境（hosted 上另有 `prepare_boost.sh` 的 Boost 1.90 私有前缀）。

### Q: 本地运行压测卡住了？

首先杀残留进程：

```bash
pkill -TERM -f unified_stress 2>/dev/null || true
```

P2 起本仓内置 `bbt/core` 依赖闭包与 `bbt/pollevent`，`libbbt_coroutine.so` 不再外链 `libbbt_core.so`；`ldd` 输出中不应再出现它：

```bash
ldd build/bin/benchmark_test/unified_stress | grep libbbt
# 期望：只有 libbbt_coroutine.so
```

如果可执行文件仍链接到旧版 `libbbt_coroutine.so`，确认其日期与最后一次构建/安装一致即可。

### Q: 如何手动触发内存检测？

GitHub → Actions → "内存检测" → "Run workflow" → Run。

### Q: 压测间隔和持续时间怎么调？

通过环境变量控制：

```bash
FATIGUE_INTERVAL=60   # reporter 采样间隔，默认 60s
INTERVAL=10           # run_parallel_stress.sh 的采样间隔
FATIGUE_INTERVAL=2 timeout -s KILL 15 ./build/bin/benchmark_test/unified_stress 10 0 0
# 2s 间隔，10s 时长，方便快速验证
```

### Q: 如何确认本地的 libbbt_coroutine.so 是最新的？

```bash
# 查看编译时间
ls -la /usr/local/lib/libbbt_coroutine.so
stat /usr/local/lib/libbbt_coroutine.so

# 查看链接的 .so 路径
ldd build/bin/benchmark_test/unified_stress | grep bbt
```

本仓源码改动重新构建后用 `shell/install.sh` 安装即可；`bbt/core` 子集与 `bbt/pollevent` 头已随本仓一同发布，不再依赖 core 仓的 `make install`。

### Q: 压测报告怎么读？

参见 `scripts/run_parallel_stress.sh` 的汇总逻辑：读取每个模块日志中该模块最后一条 `FATIGUE_METRIC` JSON，校验 `ops_total > 0`、`errors = 0`、`elapsed_s` 达到计划时长，汇总成表格。任一模块无效时结果为 FAIL。

---

## 8. 添加新测试

### 8.1 新增单元测试

**文件位置：** `unit_test/Test_<module>.cc`

**模板：**
```cpp
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <bbt/coroutine/coroutine.hpp>

BOOST_AUTO_TEST_SUITE(t_my_new_test)

BOOST_AUTO_TEST_CASE(t_basic) {
    // 测试逻辑
    BOOST_TEST(1 + 1 == 2);
}

BOOST_AUTO_TEST_SUITE_END()
```

**注册到 CMake：** 编辑 `unit_test/CMakeLists.txt`，仿照现有测试添加三行：

```cmake
add_executable(Test_my_module Test_my_module.cc)
target_link_libraries(Test_my_module ${MY_LIBS})
add_test(NAME Test_my_module COMMAND Test_my_module)
```

其中 `MY_LIBS` 当前只包含 `bbt_coroutine`。测试文件静态内嵌 Boost.Test，不能再链接
`boost_unit_test_framework` 或 `boost_test_executor_monitor`，否则会造成框架全局状态冲突。

**验证：** 重新编译后 `ctest --output-on-failure` 检查新测试是否通过。

### 8.2 新增压测模块

如果新增并发原语需要纳入 unified_stress，需要改动：

1. **`benchmark_test/unified_stress.cc`：** 添加 `stress_<new_module>()` 函数
2. **`benchmark_test/fatigue_metrics.h`：** 添加新模块的计数器字段
3. **`scripts/run_parallel_stress.sh`：** 在 `MODULES` 数组添加模块名

### 8.3 新增 benchmark

独立 benchmark 放在 `benchmark_test/benchmark_<name>.cc`，仿照 `benchmark_comutex.cc`。
编辑 `benchmark_test/CMakeLists.txt` 仿照现有 target 注册。

---

## 9. 团队角色与通知

### 角色分工

| 角色 | 职责 | CI 关注点 |
|------|------|----------|
| **developer** | 提交代码 | build-and-test pass + 本地压测自查 |
| **reviewer** | 审查 PR | CI 全部 pass，性能关键路径需附压测数据 |
| **qa** | 质量放行 | 发布 Gate 压测结果 + 内存检测报告 |
| **pm** | 发布管理 | 压测结果趋势 + 里程碑健康度 |
| **architect** | API 设计 | breaking change 影响评估 |

### 通知机制

- **PR 提交者：** CI 结果通过 GitHub Checks API 直接显示在 PR 页面
- **发布 Gate：** 压测结果在 Release Gate 的 Actions 日志与 artifact 中查看
- **定期监控：** 每周五 17:30（UTC+8）自动运行内存检测

---

## 10. 版本发布

版本发布唯一入口是 `.github/workflows/release.yml`，只能通过 GitHub Actions 的 `workflow_dispatch` 触发；Agent 和开发者不得直接创建、移动或删除 `v*` tag。Stable 发布还需通过 `release-stable` Environment 审核。

版本序列：

```text
v3.0.0-rc1 -> v3.0.0-rcN -> v3.0.0
```

RC 输入 `release_kind=rc`、`version`（尚未存在、由用户确认的合法 rcN 版本号，照抄已存在的 tag 会被 Gate 拒绝）、当前 main 的完整 `source_sha`、可选 `soak_seconds`（默认 3600）。Stable 输入 `release_kind=stable`、`version=v3.0.0`、RC 的完整 `source_sha` 和 `rc_tag`。Release Gate 会执行 Release 构建、全量 CTest、真实客户端验收、可配置时长疲劳压测，并在通过后写入性能基线，同时校验版本、main HEAD、RC tag 和 GitHub Release。

任一 Gate 失败都不会创建 tag 或 Release。Stable 只能从已验证 RC 的同一 commit 晋级；需要修复时递增 RC 序号，不移动已发布 tag。若创建后回读失败，先按 workflow run 对账，禁止盲目重试。

GitHub tag ruleset 按 bypass actor 限制创建者，不能直接指定 workflow；本仓采用发布专用 deploy key 作为 bypass actor 实现版本 tag 保护（配置与边界见 [`agent-docs/development-and-release-process.md`](../agent-docs/development-and-release-process.md) §7，含 deploy key 自身的写权限残余风险）。发布前仍须用 GitHub API 实时回读 ruleset 确认现行配置，不以此处描述为准。

## 11. 参考文档

| 文档 | 路径 | 说明 |
|------|------|------|
| CI Workflow | `.github/workflows/unit_test.yml` | CI 配置源码 |
| 内存检测 Workflow | `.github/workflows/memery_test_info.yml` | 内存检测配置 |
| 发布 Workflow | `.github/workflows/release.yml` | 发布 Gate |
| 正式 CI 说明 | `agent-docs/formal-ci.md` | 普通 CI 配方与分类/结果语义 |
| 性能门禁接线 | `agent-docs/perf-check-wiring.md` | 同 runner base/head 判定链与边界 |
| 遗留编译脚本 | `shell/workflow/unit_test/compile_code.sh` | 本地辅助入口；当前 CI 不调用 |
| 并行压测脚本 | `scripts/run_parallel_stress.sh` | 6 模块并行压测 |
| 压测运行器 | `scripts/run_fatigue.py` | 串行疲劳测试运行器 |

---

> 本文件随 CI 变更同步维护；各步骤参数（timeout、OUTDIR、artifact 名称等）以 workflow 定义为准。
