# bbtools-coroutine 性能基准测试套件

> 版本: 1.0 | 日期: 2026-06-30
>
> 契约文档: [docs/testing-contract-spec.md](../docs/testing-contract-spec.md)
> ADR: [docs/adr/0001-testing-contract.md](../docs/adr/0001-testing-contract.md)

## 概览

本目录包含 bbtools-coroutine 的性能基准测试和可靠性验证工具，覆盖 6 个核心模块的吞吐、
延迟、内存占用及并发正确性。

### 三层测试体系

```
Layer 1: 单元测试 (ctest)        ← 每次 commit 必跑，~60s
Layer 2: 冒烟测试 (Test_smoke)   ← 每次 commit 必跑，~8s
Layer 3: 疲劳测试 (unified_stress) ← PR risk 判定 + push main 必跑，1h
```

## 快速开始

### 编译

```bash
cd build
cmake .. -G Ninja -DNEED_TEST=ON -DNEED_BENCHMARK=ON
ninja -j$(nproc)
```

### 30 秒快速验证

```bash
# 全模块快跑（确认所有模块有输出）
FATIGUE_INTERVAL=10 timeout -s KILL 45 ./bin/benchmark_test/unified_stress 30 0 0
```

### 1 小时完整疲劳测试

```bash
# 并行隔离模式（推荐）
INTERVAL=60 bash scripts/run_parallel_stress.sh 3600 2
```

## 程序清单

| 程序 | 用途 | 输出格式 |
|------|------|---------|
| `unified_stress` | 全模块/单模块吞吐压测 | `FATIGUE_METRIC:{...}` JSON 行 |
| `micro_co_switch` | 协程切换延迟微基准 | `BENCH_RESULT:{...}` JSON |
| `benchmark_coroutine` | 100 万协程创建吞吐 | 文本 |
| `benchmark_chan` | Chan 读写吞吐 | 文本 |
| `benchmark_comutex` | CoMutex 正确性（不变量） | 文本 |
| `benchmark_cocond` | CoCond 通知吞吐 | 文本 |
| `benchmark_copool` | CoPool 任务吞吐 | 文本 |
| `cocond_queue_stress` | CoCond 队列压力测试 | 文本 |
| `mem_check_test` | 内存泄漏检查 | Valgrind |

## CLI 参考

### unified_stress

```
unified_stress [duration_s] [busy_us] [sleep_us] [--module=X] [--threads=N]
```

| 参数 | 默认 | 说明 |
|------|------|------|
| `duration_s` | 3600 | 测试时长（秒） |
| `busy_us` | 0 | CPU 忙等微秒数 |
| `sleep_us` | 0 | 已废弃 |
| `--module=X` | 全模块 | 单模块：`comutex\|corwmutex\|cocond\|chan\|copool\|coroutine` |
| `--threads=N` | `$(nproc)` | processer 线程数 |

环境变量:

| 变量 | 说明 |
|------|------|
| `FATIGUE_INTERVAL` | JSON 输出间隔（秒），默认 10 |
| `CO_PROC_THREADS` | 线程数（`--threads` 未指定时） |

### micro_co_switch

```
micro_co_switch [iterations] [threads]
```

测量：
- **Phase 1**: 单协程 `bbtco_yield` 往返延迟（含调度器开销）
- **Phase 2**: 协程创建吞吐

## 输出格式

### FATIGUE_METRIC JSON

```json
{
  "ts": 60.0,
  "name": "comutex",
  "elapsed_s": 60.0,
  "ops_total": 123456,
  "errors": 0,
  "lock_ops": 120000,
  "trylock_success": 3000,
  "trylock_timeout": 456,
  "lock_avg_us": 2.5,
  "lock_latency_count": 120000,
  "rlock_ops": 0,
  "wlock_ops": 0,
  "try_rlock_timeout": 0,
  "try_wlock_timeout": 0,
  "wlock_avg_us": 0,
  "wlock_latency_count": 0,
  "cond_waits": 0,
  "cond_signals": 0,
  "cond_timeouts": 0,
  "cond_avg_us": 0,
  "chan_reads": 0,
  "chan_writes": 0,
  "chan_read_blocks": 0,
  "chan_write_blocks": 0,
  "pool_tasks": 0,
  "pool_completed": 0,
  "pool_dropped": 0,
  "co_spawns": 0,
  "co_switches": 0
}
```

**字段契约**: 所有字段始终存在（非本模块指标为 0），消费者无需做 `has_key` 检查。

### BENCH_RESULT JSON

```json
{
  "name": "micro_co_switch",
  "iterations": 1000000,
  "measured_iterations": 990000,
  "threads": 8,
  "yield_roundtrip_avg_ns": 1234,
  "co_spawn_count": 100000,
  "co_spawn_avg_ns": 567
}
```

## 报告归档

长时间压测报告归档到 `tests/reports/<YYYY-MM-DD_HH-MM-SS>/`：

```
tests/reports/<timestamp>/
├── summary.json        # 汇总
├── summary.txt         # 人类可读表格
├── comutex.log         # 每模块 raw output
└── ...
```

`tests/reports/latest` → 符号链接指向最近一次。

## 性能基线

### 记录基线

```bash
# 全模块 60s 基线（2 线程）
python3 scripts/record_baseline.py record --threads=2 --dur=60

# 快速模式（每模块 ≤60s）
python3 scripts/record_baseline.py record --quick
```

基线文件: `tests/baselines/<machine>/<timestamp>_<commit>.json`

### 对比基线

```bash
# 自动对比最近两次
python3 scripts/record_baseline.py compare --latest-only

# 指定文件对比
python3 scripts/record_baseline.py compare baseline1.json baseline2.json
```

### 退化阈值（ADR D6）

| 退化幅度 | 动作 |
|---------|------|
| <10% | 记录，不拦截 |
| 10%~20% | 拦截 PR，需 ADR 说明 |
| >20% | 拦截 PR，升级为事故 |
| CoCond >30% | 拦截（CoCond 阈值放宽） |

## Sanitizer 测试

```bash
# 构建 ASAN+UBSAN 版
bash scripts/build_sanitizer.sh build

# 运行全模块（默认 10min/模块）
bash scripts/build_sanitizer.sh run

# 运行单模块
bash scripts/build_sanitizer.sh run-module comutex 600

# 清理
bash scripts/build_sanitizer.sh clean
```

## 已知陷阱与验证

测试套件覆盖的已知可靠性陷阱（详见契约文档 §3）：

| ID | 陷阱 | 验证方法 |
|----|------|---------|
| DL-01 | 持锁 await | 疲劳测试 ops 停滞检测 |
| DL-02 | 析构竞态死锁 | 2 线程 comutex 测试（已修复） |
| DL-03 | LOOP_NONBLOCK 阻塞 | 单测（已修复） |
| DL-04 | CoCond 调度饿死 | `errors` 字段持续增长检测 |
| UAF-01~04 | Use-After-Free | ASAN 构建 + 疲劳测试 |
| DR-01~02 | 数据竞争 | Chan 并发读写（TSan 不纳入 CI） |
| EX-01~03 | 异常安全 | ctest 覆盖 |

## 门禁标准

| 检查项 | 标准 | 失败动作 |
|--------|------|---------|
| ctest | 全部通过 | 拦截 |
| Test_smoke | 全部通过 | 拦截 |
| 吞吐退化 | >10% (CoCond >30%) | 拦截 |
| 死锁冻结 | ops 停滞 >2 周期 | 拦截 |
| ASAN 错误 | 任意 error | 拦截 |
| RSS 泄漏 | 持续单调增长 | 拦截 |
| Assert 失败 | 任意断言违例 | 拦截 |
