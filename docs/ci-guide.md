# bbtools-coroutine CI 使用文档与团队接入指南

> 版本: 1.0 | 作者: pm | 日期: 2026-06-30
>
> **读者**: 所有贡献者、reviewer、CI 维护者
>
> 本文档说明 bbtools-coroutine 的 CI 流水线如何触发、如何解读报告、常见错误如何处理、
> 如何添加新测试以及如何维护性能基线。

---

## 1. 快速上手

### 我是贡献者，我要提 PR

```bash
# 1. 开分支
git fetch origin && git checkout -b feat/my-change origin/main

# 2. 修改代码 + 本地验证（必做！）
cd build && cmake .. -G Ninja -DNEED_TEST=ON -DNEED_BENCHMARK=ON && ninja -j$(nproc)
ctest --output-on-failure                          # Layer 1: 单元测试 ~60s
./bin/unit_test/Test_smoke --log_level=test_suite  # Layer 1: 冒烟测试 ~8s
./bin/unit_test/Test_reliability --log_level=test_suite  # Layer 1: 可靠性 ~10s

# 3. 本地性能自检（推荐，避免 CI 才暴露退化）
python3 scripts/ci_perf_check.py --threads=2 --duration=45

# 4. push 分支 → 创建 PR
```

CI 在 push PR 后自动运行：Layer 1 编译+测试（~90s）→ Layer 2 性能回归检查（~5min）。
**合并到 main 后触发 Layer 3 1h 疲劳压测**。

### 我是 reviewer，要审 PR

检查 CI 报告中的三个关键信号：

| 信号 | 含义 | 动作 |
|------|------|------|
| ✅ 全部 pass | 无退化 | 正常 review |
| ⚠️ WARN（吞吐退化 10~20%） | 性能轻微退化 | 检查 ADR 是否附说明 |
| ❌ FAIL（>20% / 冻结 / ASAN） | 严重退化或 bug | 要求修复后再审 |

### 我是 CI 维护者，管道挂了

见 [§7 FAQ](#7-faq) 的「流水线挂了怎么排查」。

---

## 2. CI 触发机制

### 触发事件

| 事件 | 触发 | Layer 1 | Layer 2 | Layer 3 | Sanitizer |
|------|------|:-------:|:-------:|:-------:|:---------:|
| **PR 创建/更新** | `pull_request → main` | ✅ | ✅ | ❌ | ❌ |
| **push main** | `push → main` | ✅ | ❌ | ✅ | ❌ |
| **手动触发** | `workflow_dispatch` | — | — | — | ✅ |

### 运行环境

基于 **self-hosted runner**（Linux，runner 名 `txclould`），runs-on: `["self-hosted", "linux"]`。

### 超时设置

| Layer | timeout | 说明 |
|-------|---------|------|
| Layer 1 (ctest) | 5 min | 单元测试，20 个 test suite |
| Layer 2 (perf) | 15 min | 6 模块 × 45s 串行 |
| Layer 3 (stress) | 90 min | 1h 压测 + 基线记录 |
| Sanitizer | 90 min | 6 模块 × 10min |

---

## 3. 三级门禁流水线

```
Layer 1 (每次 PR/commit): 编译 → ctest → Test_smoke → Test_reliability     ~90s
Layer 2 (每次 PR):         快速性能回归检查（unified_stress 45s/模块）        ~5min
Layer 3 (push main only):  1h 并行疲劳压测 + 记录新基线                       ~70min
Layer 4 (手动):            Sanitizer 检查（ASAN+UBSAN，6 模块 × 10min）      ~1h
```

### Layer 1: 编译 + 单元测试（每次 commit 必跑）

**步骤：**
1. 编译：`cmake -G Ninja -DNEED_TEST=ON -DNEED_BENCHMARK=ON && ninja`
2. ctest：运行全部 16 个单元测试 suite
3. Test_smoke：冒烟测试，覆盖 8 个核心模块 happy-path
4. Test_reliability：可靠性测试，验证 DL/UAF/EX 陷阱清单

**失败含义：**
- 编译失败 → 代码有语法/链接错误，检查编译日志
- ctest 失败 → 某测试用例失败，检查对应 module 的 `--output-on-failure` 输出
- Test_smoke/Test_reliability 失败 → 核心模块 happy-path 破坏或已知陷阱触发

### Layer 2: 性能回归检查（每次 PR 必跑）

**运行方式：** `python3 scripts/ci_perf_check.py --threads=2 --duration=45`

每个模块独立运行 unified_stress 45s，与基准基线对比吞吐和延迟。阈值遵循 ADR D6：

| 退化幅度 | 动作 | 退出码 |
|---------|------|--------|
| <10% | 记录，不拦截 ✅ | 0 |
| 10~20% | 拦截 PR ⚠️，需 ADR 说明 | 1 |
| >20% | 拦截 PR ❌，升级为事故，需根因分析 | 2 |

**CoCond 例外：** 阈值放宽到 30%（因对调度变化高度敏感）。

CI 结果以两种形式呈现：
1. **GitHub Step Summary**：Markdown 表格（吞吐对比 + 延迟对比 + 冻结检测）
2. **GitHub Annotations**：逐模块的 `::warning` / `::error` 注解，PR Files Changed 可见

**详细说明见：** `docs/testing-contract-spec.md` §4.6

### Layer 3: 1h 疲劳压测（push main 触发）

**运行方式：** `INTERVAL=60 bash scripts/run_parallel_stress.sh 3600 2`

6 模块独立进程并行运行，每 60s 采样 JSON 指标行。压测完成自动记录新基线。

**产物：**
- 压测日志（上传 artifact，保留 7 天）
- 性能基线文件 `tests/baselines/<machine>/<timestamp>_<commit>.json`（保留 30 天）

### Layer 4: Sanitizer 检查（手动触发）

**触发方式：** GitHub Actions → `unit_test.yml` → "Run workflow" → 选择 branch

**运行：** `bash scripts/build_sanitizer.sh build && bash scripts/build_sanitizer.sh run 600`

ASAN + UBSAN 构建版统一压测，每模块 10min，检测：
- heap-use-after-free / stack-use-after-return
- buffer overflow / memory leak（ASAN）
- undefined behavior（UBSAN）

TSan（Thread Sanitizer）不纳入 CI，原因见 ADR D4。

---

## 4. 解读 CI 报告

### 4.1 Layer 2 性能回归报告

CI Step Summary 包含三张表：

#### 吞吐对比

```
| Module    | Old ops/s | New ops/s | Delta% | Threshold | Verdict |
|-----------|-----------|-----------|--------|-----------|---------|
| comutex   |     15000 |     14500 |  -3.3% |       10% | ✅ OK   |
| cocond    |      4200 |      3100 | -26.2% |       30% | ⚠️ WARN |
| coroutine |       980 |       950 |  -3.1% |       10% | ✅ OK   |
```

#### 延迟对比

```
| Module    | Old μs | New μs | Delta% | Verdict |
|-----------|--------|--------|--------|---------|
| comutex   |    2.5 |    2.6 |  +4.0% | ✅      |
| cocond    |  120.0 |  180.0 | +50.0% | ⚠️      |
```

#### 冻结检测

采样间隔内 ops_total 未增长 >2 次 → 报告为 FROZEN → 退出码 2（FAIL）。

### 4.2 GitHub Annotations

CI 在 PR Files Changed 页面的左侧 annotation 栏逐模块标注：

- `::warning` → 警告级退化（10~20%），标黄
- `::error` → 严重退化（>20%）或冻结，标红

### 4.3 CI 退出码

| 退出码 | 含义 |
|--------|------|
| 0 | PASS — 全部通过 |
| 1 | WARN — 有退化但未超阈值 |
| 2 | FAIL — 严重退化 / 冻结 / 错误 |

---

## 5. 本地运行完整测试套件

### 5.1 编译

```bash
cd /path/to/bbtools-coroutine/build
cmake .. -G Ninja -DNEED_TEST=ON -DNEED_BENCHMARK=ON
ninja -j$(nproc)
```

### 5.2 Layer 1: 单元测试

```bash
# 全部单元测试
ctest --output-on-failure

# 单个模块
./bin/unit_test/Test_comutex
./bin/unit_test/Test_chan
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

# 可靠性测试
./bin/unit_test/Test_reliability --log_level=test_suite
```

### 5.3 Layer 2: 性能回归自查

```bash
# 全模块 45s/模块 等同 CI
python3 scripts/ci_perf_check.py --threads=2 --duration=45

# 跳过 microbenchmark（更快）
python3 scripts/ci_perf_check.py --threads=2 --duration=45 --skip-micro
```

### 5.4 Layer 3: 疲劳压测

```bash
# 快速冒烟：2s 间隔，30s 全模块
FATIGUE_INTERVAL=2 timeout -s KILL 45 ./build/bin/benchmark_test/unified_stress 30 0 0

# 模块隔离：每进程一个模块（推荐）
INTERVAL=10 bash scripts/run_parallel_stress.sh 60 2

# 1h 全量（等同 CI Layer 3）
INTERVAL=60 bash scripts/run_parallel_stress.sh 3600 2

# 单模块长时间压测
FATIGUE_INTERVAL=60 ./build/bin/benchmark_test/unified_stress --module=comutex --threads=4 600 0 0
```

### 5.5 Layer 4: Sanitizer 本地运行

```bash
# 构建 ASAN+UBSAN 版
bash scripts/build_sanitizer.sh build

# 运行全部模块（每模块 10min）
bash scripts/build_sanitizer.sh run 600

# 单模块
bash scripts/build_sanitizer.sh run-module comutex 600

# 清理
bash scripts/build_sanitizer.sh clean
```

### 5.6 覆盖率测试

```bash
# 一键：构建插桩版 → 跑全部单元测试 → 生成 HTML 报告
bash scripts/coverage.sh all

# 浏览器打开
open coverage_report/index.html
```

---

## 6. 添加新测试

### 6.1 新增单元测试

**文件位�：** `unit_test/Test_<module>.cc`

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

**注册到 CMake：** 编辑 `unit_test/CMakeLists.txt`，仿照现有 `Test_chan` 添加：
```cmake
add_executable(Test_my_module Test_my_module.cc)
target_link_libraries(Test_my_module ${TEST_LIBS})
add_test(NAME Test_my_module COMMAND Test_my_module --log_level=test_suite)
```

**验证：** 重新编译后 `ctest --output-on-failure` 检查新测试是否通过。

### 6.2 新增可靠性测试

可靠性测试按陷阱 ID 命名：`t_<TRAP-ID>`（如 `t_DL_01`），添加到 `Test_reliability.cc`。

参考已有陷阱清单：`docs/testing-contract-spec.md` §3。

### 6.3 新增性能模块

如果新增并发原语需要纳入 unified_stress 的性能测试，需要改动三处：

1. **`benchmark_test/unified_stress.cc`：** 添加 `stress_<new_module>()` 函数
2. **`benchmark_test/fatigue_metrics.h`：** 添加新模块的计数器字段
3. **`scripts/ci_perf_check.py`：** 在 `MODULES` 列表中添加模块名 + 回归阈值

**⚠️ 契约变更必须：** 更新 `docs/testing-contract-spec.md` + 写 ADR。

### 6.4 添加新 benchmark

独立 benchmark（非 unified_stress）放在 `benchmark_test/benchmark_<name>.cc`，仿照 `benchmark_comutex.cc`：

```cpp
#include "fatigue_metrics.h"
static fatigue::Metrics g_metrics("my_bench");

int main() {
    g_metrics.start_reporter(10);  // 10s 一帧
    // ... run benchmark ...
    return 0;
}
```

**注册：** 编辑 `benchmark_test/CMakeLists.txt`，仿照 `benchmark_comutex` 添加 target。

---

## 7. FAQ

### Q: PR 的 CI 失败了，"编译与单元测试" 挂了怎么办？

先看 CI 日志定位是哪个阶段失败：

- **编译失败** → 检查本地是否 pass 了 `ninja -j$(nproc)`。CI 用 Ninja 编译，确认本地也用了 Ninja。
- **ctest 失败** → 展开 CI 日志看具体哪个 test case 失败，本地 `ctest --output-on-failure` 复现。
- **Test_smoke/Test_reliability 失败** → 本地运行同名测试看完整输出。

### Q: "性能回归检查" 报 WARN 怎么办？

说明某模块吞吐退化 10~20% 或延迟增长 >20%。

1. 先确认退化是否由本 PR 引入：对比 PR 前后 commit 的性能基线
2. 如果是本 PR 引入 → 检查改动是否涉及性能关键路径，考虑优化或拆分 PR
3. 如果退化可接受（如新增功能导致的必要开销）→ 附 ADR 说明，标记为预期退化
4. 如果退化由机器扰动造成 → 重跑 CI

### Q: "性能回归检查" 报 FAIL 怎么办？

吞吐退化 >20% 或检测到冻结。

1. **冻结检查：** 查看 CI Step Summary 的 "Freeze Detected" 表格。冻结 = 协程全员卡死。
2. **大幅退化：** 回退改动、修复性能问题后再推。

### Q: CI 基线不存在（"No baseline found"）怎么办？

首次运行或新 runner 机器。CI 不会因此失败（退出码 0），但会发 warning 注解。需要建立基线：

```bash
# 在 CI runner 上运行
python3 scripts/record_baseline.py record --threads=2 --dur=60 --quick
```

基线文件写入 `tests/baselines/<machine>/`，后续 PR 自动对比。

### Q: Layer 2 和 Layer 3 的区别是什么？

- **Layer 2（perf-regression）**：每次 PR 触发，45s/模块快速检查，**相对基线对比**
- **Layer 3（stress-test）**：push main 触发，1h 全量压测，**验证长时间稳定性 + 记录新基线**

Layer 2 快但可能漏过偶发冻结（~19min 的 CoCond 饥饿）：Layer 3 做兜底。

### Q: 如何手动触发 Sanitizer 检查？

GitHub → Actions → CI → "Run workflow" → 选择 branch → Run。

Sanitizer 构建 + 每模块 10min，总计 ~1h。完成后检查 artifact 中的 `asan_report.*`。

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

确认 `libbbt_core.so` 日期与最后一次 `sudo make install` 一致。如果改了 core 代码必须先 `sudo make install` 才生效。

### Q: Valgrind 报告一大串"definitely lost"？

Valgrind 在协程场景下不可靠——自定义栈切换、context switch 产生大量假阳性。2.2MB "definitely lost" 实为协程栈的正常分配。**用 RSS 时序监控替代 Valgrind：**

```bash
# 1h RSS 监控
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
"
```

判断标准：RSS 增长 < 100 MB/h 且趋势平稳 → 无泄漏。

### Q: CI 编译失败提示 ninja 找不到？

runner 上 `ninja` 未安装或不在 PATH。检查 self-hosted runner 环境——bbtools-coroutine 必须用 Ninja 编译。

---

## 8. 性能基线维护

### 8.1 基线生命周期

| 基线类型 | 存储位置 | 保留天数 | 更新频率 |
|---------|---------|---------|---------|
| PR 性能基线 | `tests/baselines/<machine>/` (git) | 永久 | push main 时更新 |
| 压测日志 artifact | GitHub Actions artifacts | 7 天 | 每次 push main |
| 性能基线 artifact | GitHub Actions artifacts | 30 天 | 每次 push main |

### 8.2 记录新基线

```bash
# 全量（每模块 60s）
python3 scripts/record_baseline.py record --threads=2 --dur=60

# 快速（每模块最多 60s）
python3 scripts/record_baseline.py record --threads=2 --dur=60 --quick

# 对比两次基线
python3 scripts/record_baseline.py compare path1 path2

# 对比最近两次
python3 scripts/record_baseline.py compare --latest-only
```

### 8.3 基线退化处理

当 CI 检测到退化时的决策树：

```
退化 <10%  → 记录，不拦截 ✅
退化 10-20% → 拦截 PR ⚠️
  ├─ 是新功能预期开销？→ 附 ADR 说明，放行
  └─ 意外退化？        → 修复后退化消失，放行
退化 >20%  → 拦截 PR ❌，升级为事故
  → 根因分析，修复后退化消失才放行
```

### 8.4 基线文件格式

每个基线文件在 `tests/baselines/<machine>/<timestamp>_<commit>.json`，格式：

```json
{
  "timestamp": "2026-06-30T14:00:00",
  "git_commit": "abc1234",
  "threads": 2,
  "modules": {
    "comutex": {"ops_per_s": 15000, "lock_avg_us": 2.5},
    ...
  }
}
```

---

## 9. 团队角色与通知

### 角色分工

| 角色 | 职责 | CI 关注点 |
|------|------|----------|
| **developer** | 提交代码 | Layer 1/2 通过 + 附 ADR（如退化） |
| **reviewer** |审查 PR | CI 全部 pass + ADR 合理性 |
| **qa** | 质量放行 | Layer 3 结果 + Sanitizer 报告 |
| **pm** | 发布管理 | 基线趋势 + 里程碑健康度 |
| **architect** | API 设计 | breaking change 影响评估 |

### 通知机制

- **PR 提交者：** CI 结果通过 GitHub Checks API 直接显示在 PR 页面
- **push main 失败：** GitHub Actions 邮件通知（取决于仓库 notification 设置）
- **定期监控：** 每周五 17:30（UTC+8）自动运行内存检测（`memery_test_info.yml`）

---

## 10. 参考文档

| 文档 | 路径 | 说明 |
|------|------|------|
| 测试契约 | `docs/testing-contract-spec.md` | 指标体系、陷阱清单、接口契约 |
| ADR | `docs/adr/0001-testing-contract.md` | 关键架构决策与 trade-off |
| Benchmark README | `benchmark_test/README.md` | 基准测试套件概览 |
| CI Workflow | `.github/workflows/unit_test.yml` | CI 配置源码 |
| bbtools-dev skill | skill: `bbtools-dev` | 编译/测试命令、已知陷阱、冻结诊断 |

---

> **最后更新**: 2026-06-30 | **下次评审**: 如有 CI 流程变更时更新
