# bbtools-coroutine CI 使用文档与团队接入指南

> 版本: 3.0 | 日期: 2026-09-09
>
> 开发、main 集成和版本发布的唯一流程真源：
> [`agent-docs/development-and-release-process.md`](../agent-docs/development-and-release-process.md)。
>
> 本文档说明 bbtools-coroutine 的 CI 流水线：如何触发、如何解读结果、常见问题处理、
> 如何添加测试、如何本地验证。

---

## 1. 快速上手

### 我是贡献者，我要提 PR

```bash
# 1. 基于最新 main 开分支
git fetch origin && git checkout -b feat/my-change origin/main

# 2. 修改代码 + 本地验证（必做！）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DNEED_BENCHMARK=ON
cmake --build build -j2
cd build
ctest --output-on-failure                          # 37 个核心测试套件，~90s
./bin/unit_test/Test_smoke --log_level=test_suite  # 冒烟测试，~8s

# 3. 推送分支 → 创建 PR
git push -u origin feat/my-change
# GitHub 上创建 PR，CI 自动运行编译+测试
```

CI 在 PR 创建/更新时只运行编译、单元测试、Smoke 和 Reliability。
**合并到 main 后运行真实客户端、性能回归，通过后再跑 1h 疲劳压测和记录基线**。

### 我是 reviewer，要审 PR

CI 通过（编译+ctest+smoke 全绿）是 merge 的前提条件。
检查 PR 页面上的 CI checks 状态：

| 信号 | 含义 | 动作 |
|------|------|------|
| ✅ `编译 & 单元测试` pass | 编译通过+全部测试通过 | 可以 review 代码 |
| ❌ required check fail | 编译、验收或性能门禁失败 | 要求修复后重推 |

**性能影响：** 性能检查放在 main；FAIL 或指标无效会阻断发布，不把环境依赖放在普通 PR 的关键路径。性能敏感变更仍应在合入前定向验证。

### 1.1 真实客户端验收（main / Release）

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

### CI 工作流：unit_test.yml

| 事件 | 触发 | 编译+测试 | 1h 压测 |
|------|------|:---------:|:-------:|
| **PR 创建/更新** | `pull_request → main` | ✅ | ❌ |
| **push main** | `push → main` | ✅ | ✅ |

### 运行环境

基于 **self-hosted runner**（Linux，runner 名 `txclould`），`runs-on: ["self-hosted", "linux"]`。

仓库公开。fork PR 会执行 PR head 里的 workflow，不能靠本仓库 YAML 的 `if` 隔离常驻 runner。硬闸门是 Actions `approval_policy=all_external_contributors`：外部贡献者的 fork workflow 默认不跑，必须人工批准。不要批准未审查的 fork workflow。受支持的贡献路径是同仓分支 PR。

### 超时设置

| 阶段 | timeout | 说明 |
|------|---------|------|
| build-and-test | 未设置 job 超时（GitHub 默认 6h） | CTest step 10min；Smoke / Reliability step 各 8min |
| real-client-acceptance | 10 min | Echo/hiredis 真实客户端验收 |
| perf-regression | 15 min | 每模块快速性能回归 |
| stress-test | 90 min | 含构建、1h 压测、基线记录；模块进程另有超时兜底 |
| release / gate | 120 min | 含构建、验收、性能检查和 1h 压测；不是实测完成时间承诺 |
| release / CTest | step 15 min | `ctest --timeout 60`，避免单测挂死吃完 120 min |
| release / 真实客户端验收 | step 10 min | Redis 缺失必须失败，不走 skip |

---

## 3. 流水线详解

```
每个 PR/push main:
  build-and-test:  编译 → ctest（37 suites）→ Test_smoke    ~90s

仅 push main:
  real-client-acceptance: Echo/hiredis 真实验收
  perf-regression: 性能基线回归门禁
  两项通过后:
  stress-test:     1h 并行疲劳压测（6 模块同时跑）           ~70min
```

### 3.1 编译与单元测试（每次 PR/push 必跑）

**步骤（见 `.github/workflows/unit_test.yml`；CI 当前使用 workflow 内联命令）：**

1. **编译：** workflow 清空并重建 `build/` 目录，执行 CMake 和 Ninja；
   `shell/workflow/unit_test/compile_code.sh` 是遗留的本地辅助脚本，不是当前 CI 入口。
2. **ctest：** `cd build && ctest --output-on-failure` — 运行全部 37 个核心测试套件
3. **冒烟测试：** `build/bin/unit_test/Test_smoke --log_level=test_suite` — 覆盖 8 个核心模块 happy-path
4. **可靠性测试：** 独立运行 `Test_reliability`。

基础测试列表（37 个）：

```
Test_coroutine_stack      Test_coroutine             Test_coroutine_api
Test_coroutine_exception  Test_scheduler_exec       Test_coroutine_cancel
Test_exception_stop       Test_hook                  Test_hook_contract
Test_hook_blocking_fd     Test_coroutine_diag        Test_hook_timeout_flags
Test_worker_stall         Test_crash_diag             Test_hook_error_matrix
Test_scheduler_stop       Test_poller                 Test_eventloop_boundary
Test_copollevent_state    Test_eventloop_backend     Test_eventloop_contract
Test_cond                 Test_g_co                  Test_macro_wrap
Test_scheduler_api        Test_chan                  Test_coselect
Test_comutex              Test_lockguard              Test_rwlock_guard
Test_defer                Test_profiler               Test_co_rwmutex
Test_coevent              Test_copool                 Test_smoke
Test_reliability
```

真实客户端验收是独立 CI job，不属于基础 37 项列表；本地同时启用 `NEED_EXAMPLE=ON` 时会额外注册 `Test_real_clients`。

**失败含义：**
- 编译失败 → 代码有语法/链接错误，检查编译日志
- ctest 失败 → 某测试用例失败，检查对应模块的 `--output-on-failure` 输出
- Test_smoke 失败 → 核心模块 happy-path 被破坏

### 3.2 1h 并行疲劳压测（仅 push main）

**条件：** `github.event_name == 'push' && github.ref == 'refs/heads/main'`

**运行方式：** `INTERVAL=60 bash scripts/run_parallel_stress.sh 3600 2`

6 个模块独立进程并行运行：
- `comutex, corwmutex, cocond, chan, copool, coroutine`
- 每模块 2 个 processer 线程，60s 间隔采样
- job 超时 90min；模块进程在计划时长后 120s 发送 TERM，再给 30s 退出兜底

**产物：**
- 汇总报告 `tests/reports/<timestamp>/summary.txt` — 各模块最终 ops + errors
- 每模块独立日志 `tests/reports/<timestamp>/<module>.log`
- CI 自动上传 `stress-test-report` artifact，保留 7 天；本地运行时报告仍只写入 `tests/reports/`

### 3.3 性能回归门禁（#310）

**三级结构：**
- main push（Layer 2 `perf-regression`）：`ci_perf_check.py --threads=2 --dur=45`，
  与最近可比基线对比，只检查不写基线。
- main push（Layer 3 尾部）：1h 压测成功后 `record_baseline.py record`
  生成新基线，并 `trend` 检查连续退化，最后推送到长期分支。
- 基线长期记录：orphan 分支 `perf-baseline`（目录 `tests/baselines/<machine>/`），
  artifact `performance-baseline` 仅作传输副本。main 检查先 fetch 该分支再比较，
  分支缺失时显式 `NO_COMPARABLE_BASELINE`。

**判定阈值（沿用 ADR D6）：**
| 模块 | WARN | FAIL（--gate-enabled） |
|------|------|------|
| 普通模块 | 退化 ≥10% | 退化 ≥20% |
| cocond | 退化 ≥30% | 退化 ≥40% |

CoCond 放宽原因：其 ops 由 frame/timeout 定时器节拍驱动，对调度抖动和
runner 时钟噪声天然敏感，10% 会大量误报（历史压测观察）。

**`ci_perf_check.py` 的 verdict 与退出码（main 沿用，发布额外收紧）：**
- `PASS` / `NO_COMPARABLE_BASELINE` → exit 0。无基线、基线损坏、环境指纹
  （machine/cpu/内存/编译器/cmake/ninja/build type/线程数）任一不一致，
  一律 `NO_COMPARABLE_BASELINE`，**不伪装成性能通过**，也不做静默比较。
- `WARN` → exit 0 + GitHub `::warning::` 注解和 summary 显式标注。不阻塞
  合并，但不得当作 PASS。
- `FAIL` / `METRIC_INVALID`（timeout/crash/zero ops/缺字段）→ exit 2，
  步骤红叉。**main 性能检查启用 `--gate-enabled`**：吞吐退化 ≥20%（CoCond ≥40%）
  使 main CI 失败并阻断发布；10%~20% 仍是 WARN。

**main 与发布的区别：** main 对 `NO_COMPARABLE_BASELINE` 保持 exit 0，并显示 warning；它只说明缺少性能比较证据，不是 PASS。Release Gate 仅接受六模块均为 `PASS` / `WARN` 且 `errors=0`；无基线、损坏或指纹不一致均阻断发布。

**Release 基线迁移：** 旧流程记录的空 `build_type` 与显式 `Release` 不可比。首个迁移 PR 允许显式不可比状态合入；合入后 main 完成 1h 长测并通过 record 写入闸门，生成同 runner / 参数的 Release 基线，再执行 RC Gate。不手改基线指纹、不伪造数据、不关闭 required check。

**故障分类：** runner/环境故障看「编译 & 单元测试」是否同挂与 `NO_COMPARABLE_BASELINE`
的 reason 键；harness 故障 = `METRIC_INVALID`（指标缺失/进程崩溃）；代码性能回退 =
指纹一致下的 WARN/FAIL delta。三者不混算。

**基线写入闸门：** `record_baseline.py record` 在任一模块 no_data/超时/指标无效时
拒绝落盘（exit 2）；失败、取消、超时的 run 不会覆盖好基线。

**趋势与修复义务：** `record_baseline.py trend` 比较最近 4 份基线，逐次下降且累计
≥15% 时告警。**连续两次 main 或发布前确认性能回退，必须创建专项修复 Issue**；
单次异常先复测并记录环境。

**基线保留规则：** 推送步骤自动裁剪每机器目录至最近 20 份（约 20 次 main）；
关键发布基线打 tag（在 `perf-baseline` 分支上，如 `baseline-v2.1.0`）长期保留。

**本地复现：**

```bash
# 构建（Release，与 CI 同参数）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNEED_TEST=ON -DNEED_BENCHMARK=ON
cmake --build build -j2

# 冒烟（不比较基线）
python3 scripts/ci_perf_check.py --module=comutex --threads=2 --dur=15 --no-baseline-compare

# 快速回归（自动取本机最新基线比较；无基线则 NO_COMPARABLE_BASELINE）
python3 scripts/ci_perf_check.py --threads=2 --dur=45

# 记录基线（仅 main 长测语义；本地试验用 --output 写到别处）
python3 scripts/record_baseline.py record --threads=2 --dur=60 --quick

# 趋势检查
python3 scripts/record_baseline.py trend
```

门禁契约测试：`python3 -m unittest discover -s scripts/ci -p 'test_*.py'`。

---

## 4. 内存检测（memery_test_info.yml）

独立于 unit_test.yml 的工作流，**每周五 17:30（UTC+8）自动运行**，也支持手动触发。

**步骤：**
1. `compile_testfile.sh` — 编译 `benchmark_test/mem_check_test.cc`
2. `do_valgrind_memcheck.sh` — Valgrind memcheck 检测内存泄漏
3. 打包结果 `memcheck_result.tar.gz`

**⚠️ Valgrind 在协程场景的限制：** 自定义栈切换、context switch 会产生大量假阳性（实测 2.3M+ errors）。Valgrind 报告需人工甄别，或改用 RSS 时序监控（见 §7 FAQ）。

---

## 5. 解读 CI 结果

### 5.1 GitHub Checks

PR 的必需检查只有 `编译 & 单元测试`；其余 job 在 PR 跳过，在 main 的 Actions run 检查：

- ✅ **`编译 & 单元测试`** — 绿色 = 编译、CTest、Smoke、Reliability 全部通过
- main 集成后才检查 **`真实客户端验收`** 与 **`性能回归检查`**
- ❌ 红色 = 失败，点击展开查看具体失败的 step 日志

### 5.2 ctest 输出解读

ctest 失败时展开 CI 日志中的 "ctest" step，查看具体失败的 test suite 和断言消息。

示例输出：
```
Test project /path/to/build
    Start 1: Test_coroutine_stack
1/37 Test #1: Test_coroutine_stack .............   Passed    0.05 sec
    Start 2: Test_coroutine
2/37 Test #2: Test_coroutine ..................   Passed    0.11 sec
...
100% tests passed, 0 tests failed out of 37
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

**性能回归：** CI 通过 `perf-regression` 自动执行基线对比。`PASS` 正常，`WARN` 会显式标注但不阻塞，`FAIL`、`METRIC_INVALID` 和无可比基线按流程分类处理；不得把 `WARN` 说成 `PASS`。

---

## 6. 本地运行测试

### 6.1 编译

```bash
cmake -S /path/to/bbtools-coroutine -B /path/to/bbtools-coroutine/build \
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DNEED_BENCHMARK=ON
cmake --build /path/to/bbtools-coroutine/build -j2
cd /path/to/bbtools-coroutine/build
```

### 6.2 单元测试

```bash
# 基础单元测试（37 个 suite）
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

### 6.5 覆盖率测试

```bash
# 一键：构建插桩版 → 跑全部单元测试 → 生成 HTML 报告
bash scripts/coverage.sh all

# 浏览器打开
open coverage_report/index.html
```

### 6.6 Sanitizer 本地诊断

ASAN+UBSAN 用于检测 use-after-free、buffer overflow 等：

```bash
# 构建
mkdir build_san && cd build_san
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug -DNEED_BENCHMARK=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
ninja -j$(nproc)

# 运行（约 2× 性能开销）
ASAN_OPTIONS=detect_leaks=0:halt_on_error=0 \
  ./bin/benchmark_test/unified_stress --module=comutex --threads=4 600 0 0
```

### 6.7 内存泄漏检测（RSS 监控）

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

- **编译失败** → 检查本地是否 pass 了 `ninja -j$(nproc)`。CI 用 Ninja 编译，确认本地也用了 Ninja。
- **ctest 失败** → 展开 CI 日志看具体哪个 test case 失败，本地 `ctest --output-on-failure` 复现。
- **Test_smoke 失败** → 本地运行 `./bin/unit_test/Test_smoke --log_level=test_suite` 看完整输出。

确认本地也失败后修复，确保本地通过后再推。

### Q: push main 后的 1h 压测失败了怎么办？

检查 `tests/reports/` 下的压测日志：

1. **某模块 ops = 0** → 该模块冻结，检查是代码改动引入还是 runner 环境问题
2. **进程 crash / segfault** → 用 core dump 分析：`gdb build/bin/benchmark_test/unified_stress /tmp/core.*`
3. **进程被 OOM kill** → runner 内存不足，减少并发模块数或线程数

### Q: 如何本地复现 CI 的测试环境？

CI 的编译脚本等价于：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DNEED_BENCHMARK=ON
cmake --build build -j2
cd build
ctest --output-on-failure
./bin/unit_test/Test_smoke --log_level=test_suite
```

本地跑一遍等价流程即可复现 CI 环境。

### Q: 本地运行压测卡住了？

首先杀残留进程：

```bash
pkill -TERM -f unified_stress 2>/dev/null || true
```

如果怀疑是 bbtools-core .so 版本不对（改了 core 但 coroutine 链接了旧版）：

```bash
ldd build/bin/benchmark_test/unified_stress | grep libbbt
ls -la /usr/local/lib/libbbt_*
```

确认 `libbbt_core.so` 日期与最后一次 `sudo make install` 一致。改了 core 代码必须先 `sudo make install` 才生效。

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

### Q: 如何确认本地的 core.so 是最新的？

```bash
# 查看编译时间
ls -la /usr/local/lib/libbbt_core.so
stat /usr/local/lib/libbbt_core.so

# 查看链接的 .so 路径
ldd build/bin/benchmark_test/unified_stress | grep bbt
```

如果 bbtools-core 有改动，必须在 core 仓库执行 `sudo make install` 更新系统安装的 .so。

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
| **qa** | 质量放行 | push main 后 1h 压测结果 + 内存检测报告 |
| **pm** | 发布管理 | 压测结果趋势 + 里程碑健康度 |
| **architect** | API 设计 | breaking change 影响评估 |

### 通知机制

- **PR 提交者：** CI 结果通过 GitHub Checks API 直接显示在 PR 页面
- **push main 后：** 1h 压测结果在 Actions 日志中查看
- **定期监控：** 每周五 17:30（UTC+8）自动运行内存检测

---

## 10. 版本发布

版本发布唯一入口是 `.github/workflows/release.yml`，只能通过 GitHub Actions 的 `workflow_dispatch` 触发；Agent 和开发者不得直接创建、移动或删除 `v*` tag。Stable 发布还需通过 `release-stable` Environment 审核。

版本序列：

```text
v3.0.0-rc1 -> v3.0.0-rcN -> v3.0.0
```

RC 输入 `release_kind=rc`、`version=v3.0.0-rc1`、当前 main 的完整 `source_sha`。Stable 输入 `release_kind=stable`、`version=v3.0.0`、RC 的完整 `source_sha` 和 `rc_tag`。Release Gate 会执行 Release 构建、全量 CTest、真实客户端验收、1h 疲劳压测，并校验版本、main HEAD、RC tag 和 GitHub Release。

任一 Gate 失败都不会创建 tag 或 Release。Stable 只能从已验证 RC 的同一 commit 晋级；需要修复时递增 RC 序号，不移动已发布 tag。若创建后回读失败，先按 workflow run 对账，禁止盲目重试。

GitHub tag ruleset 按 bypass actor 限制创建者，不能直接指定 workflow；若未配置专用 GitHub App，不能把“仅 release workflow 可建 tag”当作已落地硬保护。详细规则见 [`agent-docs/development-and-release-process.md`](../agent-docs/development-and-release-process.md)。

## 11. 参考文档

| 文档 | 路径 | 说明 |
|------|------|------|
| CI Workflow | `.github/workflows/unit_test.yml` | CI 配置源码 |
| 内存检测 Workflow | `.github/workflows/memery_test_info.yml` | 内存检测配置 |
| 遗留编译脚本 | `shell/workflow/unit_test/compile_code.sh` | 本地辅助入口；当前 CI 不调用 |
| 并行压测脚本 | `scripts/run_parallel_stress.sh` | 6 模块并行压测 |
| 覆盖率脚本 | `scripts/coverage.sh` | 覆盖率报告生成 |
| 压测运行器 | `scripts/run_fatigue.py` | 串行疲劳测试运行器 |
| bbtools-dev skill | skill: `bbtools-dev` | 编译/测试命令、已知陷阱、冻结诊断 |
| openspec | `openspec/specs/` | 各模块接口契约规范 |

---

> **最后更新**: 2026-09-09 | **下次评审**: CI 流程变更时更新
