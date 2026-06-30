# bbtools-coroutine CI 使用文档与团队接入指南

> 版本: 2.0 | 日期: 2026-06-30
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
cd build && cmake .. -G Ninja -DNEED_TEST=ON -DNEED_BENCHMARK=ON && ninja -j$(nproc)
ctest --output-on-failure                          # 16 个测试套件，~60s
./bin/unit_test/Test_smoke --log_level=test_suite  # 冒烟测试，~8s

# 3. 推送分支 → 创建 PR
git push -u origin feat/my-change
# GitHub 上创建 PR，CI 自动运行编译+ctest+冒烟
```

CI 在 PR 创建/更新时自动运行编译+单元测试（~90s）。
**合并到 main 后触发 1h 疲劳压测**。

### 我是 reviewer，要审 PR

CI 通过（编译+ctest+smoke 全绿）是 merge 的前提条件。
检查 PR 页面上的 CI checks 状态：

| 信号 | 含义 | 动作 |
|------|------|------|
| ✅ build-and-test pass | 编译通过+全部测试通过 | 可以 review 代码 |
| ❌ build-and-test fail | 编译失败或测试失败 | 要求修复后重推 |

**性能影响：** CI 不做自动性能回归检测。如果 PR 涉及性能关键路径（锁、调度器、chan），reviewer 应要求提交者附本地压测对比数据。

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

### 超时设置

| 阶段 | timeout | 说明 |
|------|---------|------|
| build-and-test | 6h (默认) | 编译+ctest+smoke，实际 ~90s |
| stress-test | 90 min | 1h 压测 + 30s 编译启动 |

---

## 3. 流水线详解

```
每个 PR/push main:
  build-and-test:  编译 → ctest（16 suites）→ Test_smoke    ~90s

仅 push main:
  stress-test:     1h 并行疲劳压测（6 模块同时跑）           ~70min
```

### 3.1 编译与单元测试（每次 PR/push 必跑）

**步骤（见 `.github/workflows/unit_test.yml`）：**

1. **编译：** `shell/workflow/unit_test/compile_code.sh` —
   清空并重建 `build/` 目录，执行 `cmake -G Ninja -DNEED_TEST=ON -DNEED_BENCHMARK=ON && ninja -j$(nproc)`
2. **ctest：** `cd build && ctest --output-on-failure` — 运行全部 16 个测试套件
3. **冒烟测试：** `build/bin/unit_test/Test_smoke --log_level=test_suite` — 覆盖 8 个核心模块 happy-path

**单元测试列表（16 个）：**

```
Test_coroutine_stack   Test_coroutine   Test_coroutine_exception   Test_hook
Test_poller            Test_cond        Test_g_co                  Test_chan
Test_comutex           Test_lockguard   Test_rwlock_guard          Test_defer
Test_co_rwmutex        Test_coevent     Test_copool                Test_smoke
```

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
- 超时 90min（1h 压测 + 30s 缓冲）

**产物：**
- 汇总报告 `tests/reports/<timestamp>/summary.txt` — 各模块最终 ops + errors
- 每模块独立日志 `tests/reports/<timestamp>/<module>.log`

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

CI 结果直接显示在 PR 页面的 **Checks** 区域：

- ✅ **build-and-test / 编译 & 单元测试 & 冒烟** — 绿色 = 全部通过
- ❌ 红色 = 失败，点击展开查看具体失败的 step 日志

### 5.2 ctest 输出解读

ctest 失败时展开 CI 日志中的 "ctest" step，查看具体失败的 test suite 和断言消息。

示例输出：
```
Test project /path/to/build
    Start 1: Test_coroutine_stack
1/16 Test #1: Test_coroutine_stack .............   Passed    0.05 sec
    Start 2: Test_coroutine
2/16 Test #2: Test_coroutine ..................   Passed    0.11 sec
...
100% tests passed, 0 tests failed out of 16
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
- ops > 0 且 errors = 0 → 正常
- ops = 0 → 冻结（协程全员卡死），需排查
- errors > 0 → CoCond 的 timeout 丢失，检查调度公平性

---

## 6. 本地运行测试

### 6.1 编译

```bash
cd /path/to/bbtools-coroutine/build
cmake .. -G Ninja -DNEED_TEST=ON -DNEED_BENCHMARK=ON
ninja -j$(nproc)
```

### 6.2 单元测试

```bash
# 全部单元测试（16 个 suite）
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
cmake .. -DCMAKE_BUILD_TYPE=Debug -DNEED_BENCHMARK=ON \
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

### Q: PR 的 CI 失败了，"编译 & 单元测试 & 冒烟" 挂了怎么办？

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
cd build
cmake .. -G Ninja -DNEED_TEST=ON -DNEED_BENCHMARK=ON
ninja -j$(nproc)
ctest --output-on-failure
./bin/unit_test/Test_smoke --log_level=test_suite
```

本地跑一遍等价流程即可复现 CI 环境。

### Q: 本地运行压测卡住了？

首先杀残留进程：

```bash
pkill -9 -f unified_stress 2>/dev/null
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

参见 `scripts/run_parallel_stress.sh` 的汇总逻辑：读取每个模块日志最后一行 `FATIGUE_METRIC` JSON，提取 `ops_total` 和 `errors`，汇总成表格。ops_total > 0 且 errors = 0 = PASS。

---

## 8. 添加新测试

### 8.1 新增单元测试

**文件位置：** `unit_test/Test_<module>.cc`

**模板：**
```cpp
#include <boost/test/unit_test.hpp>
#include <bbt/coroutine/coroutine.hpp>

BOOST_AUTO_TEST_SUITE(t_my_new_test)

BOOST_AUTO_TEST_CASE(t_basic) {
    // 测试逻辑
    BOOST_TEST(some_condition);
}

BOOST_AUTO_TEST_SUITE_END()
```

**注册到 CMake：** 编辑 `unit_test/CMakeLists.txt`，仿照现有测试添加三行：

```cmake
add_executable(Test_my_module Test_my_module.cc)
target_link_libraries(Test_my_module ${MY_LIBS})
add_test(NAME Test_my_module COMMAND Test_my_module)
```

其中 `MY_LIBS` 变量统一管理依赖（`bbt_coroutine` + `boost_unit_test_framework`）。

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

## 10. 参考文档

| 文档 | 路径 | 说明 |
|------|------|------|
| CI Workflow | `.github/workflows/unit_test.yml` | CI 配置源码 |
| 内存检测 Workflow | `.github/workflows/memery_test_info.yml` | 内存检测配置 |
| 编译脚本 | `shell/workflow/unit_test/compile_code.sh` | CI 编译入口 |
| 并行压测脚本 | `scripts/run_parallel_stress.sh` | 6 模块并行压测 |
| 覆盖率脚本 | `scripts/coverage.sh` | 覆盖率报告生成 |
| 压测运行器 | `scripts/run_fatigue.py` | 串行疲劳测试运行器 |
| bbtools-dev skill | skill: `bbtools-dev` | 编译/测试命令、已知陷阱、冻结诊断 |
| openspec | `openspec/specs/` | 各模块接口契约规范 |

---

> **最后更新**: 2026-06-30 | **下次评审**: CI 流程变更时更新
