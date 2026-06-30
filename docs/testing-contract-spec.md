# bbtools-coroutine 测试契约规格

> 版本: 1.0 | 日期: 2026-06-30
> 角色: 架构师输出，定义 qa 任务 (t_b1cb4f82 / t_fa8d3d49) 的实现契约
> 关联: [ADR-0001](adr/0001-testing-contract.md)

---

## 1. 范围与目标

本规格定义 bbtools-coroutine 的性能基准测试套件契约，覆盖 6 个核心模块：

| 模块 | 并发原语 | 压测关注点 |
|------|---------|-----------|
| coroutine | 协程基础 | 创建/销毁吞吐、上下文切换延迟 |
| comutex | CoMutex | Lock/UnLock/TryLock 吞吐、锁竞争延迟 |
| corwmutex | CoRWMutex | 读写锁吞吐、写者饥饿检测 |
| cocond | CoCond | Wait/NotifyAll 吞吐、调度公平性 |
| chan | Chan | 读写吞吐、阻塞/非阻塞模式 |
| copool | CoPool | 任务提交/完成吞吐 |

---

## 2. 性能指标体系

### 2.1 吞吐 (Throughput)

| 指标 | 单位 | 模块 | 测量方法 |
|------|------|------|---------|
| `ops_total` | ops | 全部 | 每次操作完成 +1 |
| `ops_per_s` | ops/s | 全部 | ops_total / elapsed_s |
| `lock_ops` | ops | comutex | Lock()+UnLock() 完成一次 |
| `wlock_ops` | ops | corwmutex | WLock()+WUnLock() 完成一次 |
| `rlock_ops` | ops | corwmutex | RLock()+RUnLock() 完成一次 |
| `cond_waits` | ops | cocond | Wait() 返回次数 |
| `cond_signals` | ops | cocond | NotifyAll() 调用次数 |
| `chan_reads` | ops | chan | Read() 成功次数 |
| `chan_writes` | ops | chan | Write() 成功次数 |
| `pool_tasks` | ops | copool | 任务提交数 |
| `pool_completed` | ops | copool | 任务完成数 |
| `co_spawns` | ops | coroutine | 协程创建数 |
| `co_switches` | ops | coroutine | 上下文切换数 |

### 2.2 延迟 (Latency)

| 指标 | 单位 | 模块 | 测量方法 |
|------|------|------|---------|
| `lock_avg_us` | μs | comutex | Lock() 等待时间 (t_lock - t_request) |
| `wlock_avg_us` | μs | corwmutex | WLock() 等待时间 |
| `cond_avg_us` | μs | cocond | Wait() 唤醒延迟 |
| `yield_roundtrip_avg_ns` | ns | coroutine | bbtco_yield → resume 往返 |

所有延迟测量使用 `std::chrono::steady_clock` (CLOCK_MONOTONIC)，包含前 1% warmup 排除。

### 2.3 调度速率

| 指标 | 单位 | 模块 | 说明 |
|------|------|------|------|
| `co_switches` | ops | coroutine | 总上下文切换数 (间接度量调度活跃度) |

### 2.4 锁竞争

| 指标 | 单位 | 模块 | 说明 |
|------|------|------|------|
| `trylock_success` | ops | comutex | TryLock 成功次数 |
| `trylock_timeout` | ops | comutex | TryLock 超时次数 |
| `try_rlock_timeout` | ops | corwmutex | TryRLock 超时次数 |
| `try_wlock_timeout` | ops | corwmutex | TryWLock 超时次数 |
| `cond_timeouts` | ops | cocond | WaitFor 超时次数 |
| `chan_read_blocks` | ops | chan | Read 阻塞次数 |
| `chan_write_blocks` | ops | chan | Write 阻塞次数 |

### 2.5 内存占用

| 指标 | 方法 | 频率 |
|------|------|------|
| RSS (VmRSS) | /proc/PID/status 周期采样 | 30s |
| 协程栈总量 | n_coroutines × 16KB | 启动时计算 |
| 泄漏判定 | RSS 持续单调增长 → 拦截 | 人工/LLM 审查 |

### 2.6 回归阈值 (ADR D6)

| 退化幅度 | 动作 |
|---------|------|
| <10% | 记录，不拦截 |
| 10%~20% | 拦截 PR，需 ADR 说明 |
| >20% | 拦截 PR，升级为事故 |
| CoCond >30% | 拦截 (CoCond 阈值放宽) |
| co_switch 延迟 >50% | 拦截 (微观指标敏感) |

---

## 3. 可靠性陷阱清单

### 3.1 死锁 (DL-xx)

| ID | 陷阱 | 触发场景 | 验证方法 |
|----|------|---------|---------|
| DL-01 | 持锁 await | Lock() 内 yield，调度器饥饿 | 疲劳测试 ops 停滞检测 |
| DL-02 | 析构竞态死锁 | Trigger 持锁析构 shared_ptr | 2 线程 comutex (已修复，ASIO) |
| DL-03 | LOOP_NONBLOCK 阻塞 | ASIO run_one 阻塞 scheduler | 单测已覆盖 (已修复) |
| DL-04 | CoCond 调度饿死 | 200 waiter 无 yield 重入 | `errors` 字段持续增长检测 |

### 3.2 Use-After-Free (UAF-xx)

| ID | 陷阱 | 触发场景 | 验证方法 |
|----|------|---------|---------|
| UAF-01 | CoWaiter shared_ptr 竞态 | Notify 读 m_co_event vs Wait 写 null | ASAN + 疲劳测试 |
| UAF-02 | CoPollEvent ASIO 析构 | Processer 线程析构 Event vs Scheduler poll | ASAN + 疲劳测试 |
| UAF-03 | ASIO 回调 weakthis 过期 | Event 析构后回调仍触发 | ASAN |
| UAF-04 | Processer dangling ptr | delete 后指针悬空 | ASAN + 疲劳测试 |

### 3.3 数据竞争 (DR-xx)

| ID | 陷阱 | 触发场景 | 验证方法 |
|----|------|---------|---------|
| DR-01 | Chan size() 无锁 | 多 processer 并发 push/pop | TSan (不纳入 CI) |
| DR-02 | Chan TryRead 超时未递减 | 虚假唤醒后 total 超时 | 单测 |

### 3.4 内存泄漏 (ML-xx)

| ID | 陷阱 | 触发场景 | 验证方法 |
|----|------|---------|---------|
| ML-01 | 协程栈未回收 | 长时间运行累积 | RSS 趋势审查 |
| ML-02 | Event 对象泄漏 | 异常路径未清理 | Valgrind/ASAN |
| ML-03 | CoPool 队列残留 | Stop 后未清空 | RSS 趋势审查 |

### 3.5 未定义行为 (UB-xx)

| ID | 陷阱 | 触发场景 | 验证方法 |
|----|------|---------|---------|
| UB-01 | std::vector<atomic_int>::resize | atomic 不可拷贝 | 编译期捕获 |
| UB-02 | 协程内 usleep | 阻塞调度器线程 | Code review + 静态分析 |
| UB-03 | 栈变量引用悬空 | bbtco [&] 捕获后函数返回 | ASAN |

### 3.6 异常安全 (EX-xx)

| ID | 陷阱 | 触发场景 | 验证方法 |
|----|------|---------|---------|
| EX-01 | CoCond waiter 异常 | 异常传播后 waiter 残留 | ctest (已修复 PR#205) |
| EX-02 | YieldWithCallback 锁管理 | callback 中操作锁 | 单测 (已修复 PR#207) |
| EX-03 | CoLockGuard 析构 UnLock | mutex 已失效 | ctest |

---

## 4. 测试套件接口契约

### 4.1 程序清单

| 程序 | 类型 | 输出格式 | 契约参考 |
|------|------|---------|---------|
| `unified_stress` | 吞吐压测 | `FATIGUE_METRIC:{...}` | §4.2 |
| `micro_co_switch` | 微基准 | `BENCH_RESULT:{...}` | §4.3 |
| `cocond_queue_stress` | 队列压力 | `FATIGUE_METRIC:{...}` | §4.4 |

### 4.2 unified_stress 契约

```
unified_stress [duration_s] [busy_us] [sleep_us] [--module=X] [--threads=N]
```

| 参数 | 默认 | 说明 |
|------|------|------|
| `duration_s` | 3600 | 测试时长 (秒) |
| `busy_us` | 0 | CPU 忙等微秒数 (模拟工作负载) |
| `sleep_us` | 0 | 已废弃 (使用 bbtco_sleep) |
| `--module=X` | 全模块 | 单模块隔离模式 |
| `--threads=N` | $(nproc) | processer 线程数 |

环境变量:
- `FATIGUE_INTERVAL` (int, default 10) — JSON 输出间隔 (秒)
- `CO_PROC_THREADS` (int) — 线程数备选

**输出契约:** 每 `FATIGUE_INTERVAL` 秒向 stdout 输出一行:
```
FATIGUE_METRIC:{...JSON...}\n
```

JSON Schema (所有字段始终存在，非本模块指标为 0):
```json
{
  "ts": 10.0,
  "name": "comutex",
  "elapsed_s": 10.0,
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

### 4.3 micro_co_switch 契约

```
micro_co_switch [iterations] [threads]
```

**Phase 1:** 单协程 `bbtco_yield` 往返延迟 (前 1% warmup 排除)
**Phase 2:** 100K 协程创建吞吐

输出:
```
BENCH_RESULT:{...JSON...}\n
```

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

### 4.4 cocond_queue_stress 契约

```
cocond_queue_stress [duration_s] [threads]
```

模拟高并发 Wait/NotifyAll 场景，测量队列公平性与错误率。

### 4.5 sanitizer 测试

```bash
bash scripts/build_sanitizer.sh build         # 构建 ASAN+UBSAN 版
bash scripts/build_sanitizer.sh run [dur]     # 全模块 (默认 600s/模块)
bash scripts/build_sanitizer.sh run-module <m> # 单模块
bash scripts/build_sanitizer.sh clean         # 清理
```

**注意:** TSan 不纳入 CI gate (ADR D4)，因协程栈切换产生大量 false positive。

---

## 5. 基线管理

### 5.1 记录基线

```bash
python3 scripts/record_baseline.py record --threads=2 --dur=60
python3 scripts/record_baseline.py record --quick  # 每模块 ≤60s
```

基线文件: `tests/baselines/<machine>/<timestamp>_<commit>.json`

### 5.2 对比基线

```bash
python3 scripts/record_baseline.py compare --latest-only
python3 scripts/record_baseline.py compare old.json new.json
```

退化阈值见 §2.6。

---

## 6. 报告归档

```
tests/reports/<YYYY-MM-DD_HH-MM-SS>/
├── summary.json        # 汇总 (机器可读)
├── summary.md          # 汇总 (人类可读)
├── summary.txt         # 紧凑表格
├── <module>.log        # 每模块 raw output
└── rss_trend.json      # RSS 时序 (可选)
```

`tests/reports/latest` → 符号链接指向最近一次。

---

## 附录 A: 字段命名约定

- 所有 JSON key 使用 snake_case
- 延迟字段: `<op>_avg_us` (整数或浮点)
- 计数/累计: `<op>_count` (整数)
- 时间戳: `ts`, `elapsed_s` (浮点，秒)
- 模块标识: `name` (string)
