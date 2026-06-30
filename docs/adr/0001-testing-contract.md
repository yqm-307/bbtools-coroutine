# ADR-0001: 性能基准测试套件接口设计

> 状态: 已批准 | 日期: 2026-06-30
> 决策者: 架构师 (t_ff8f2a17)
> 关联: [测试契约规格](../testing-contract-spec.md)

---

## 背景

bbtools-coroutine 的测试体系之前依赖:
1. 各模块独立的 fatigue_*.cc 程序 (格式不一致)
2. unified_stress.cc 全模块压测 (耦合在单一进程)
3. 缺乏性能基准基线管理与退化检测
4. 缺乏标准化的 sanitizer 测试流程

需要定义统一的测试套件接口，使测试可重复、结果可比较、退化可自动检测。

---

## 决策

### D1: JSON 输出采用 all-fields-present 策略

**决策:** 每条 FATIGUE_METRIC JSON 包含全部 28 个字段，非本模块指标填 0。

**理由:**
- 消费者 (Python runner, CI script) 无需做 `has_key` 检查
- 新字段加入时老 parser 不受影响 (向后兼容)
- 牺牲少量输出体积 (~800B × 1 模块 vs ~1KB × 6 模块 = 20% overhead)

**替代方案:** per-module 稀疏 JSON (仅输出本模块字段) — 拒绝，因 parser 复杂度显著增加。

---

### D2: 模块隔离通过独立进程实现

**决策:** `--module=X` 模式为每模块启动独立 unified_stress 进程，OS 调度保证公平。

**理由:**
- 避免 CoCond 饥饿拖死其他模块 (单进程已知问题)
- 进程隔离防止共享状态污染
- 与现有 `run_parallel_stress.sh` 工作流一致

**替代方案:** 单进程内部线程隔离 — 已有 MLFQ + 全局轮询修复，但公平性仍不如 OS CFS。

---

### D3: 性能基线按机器维度管理

**决策:** 基线存储路径为 `tests/baselines/<machine_slug>/<timestamp>_<commit>.json`。

**理由:**
- 不同硬件 (CPU/memory/核心数) 绝对性能不可比
- machine_slug = hostname (去特殊字符)
- commit 短哈希定位代码版本

**替代方案:** 单一全局基线 — 拒绝，因开发者机器 (8 核) vs CI runner 性能差异大。

---

### D4: TSan 不纳入 CI gate

**决策:** ThreadSanitizer 构建不纳入 CI 必过门禁，改为手动运行。

**理由:**
- 协程自定义栈切换产生大量 TSan false positive
- 协程上下文切换不经过 pthread 原语，TSan 无法正确追踪 happens-before 关系
- ASAN+UBSAN 组合覆盖 use-after-free / buffer overflow / UB

**替代方案:** 强制 TSan 通过 → 不现实，false positive 太多。

---

### D5: 可靠性陷阱采用 trap-ID 命名约定

**决策:** 每个已知陷阱分配唯一 ID: `<CATEGORY>-<NN>` (如 DL-01, UAF-02, DR-01)。

**理由:**
- 可追溯到契约文档和修复 PR
- 便于在测试报告中引用 ("DL-04 detected by cocond errors trend")
- 类别前缀 (DL/UAF/DR/ML/UB/EX) 自解释

**替代方案:** 无编号 — 拒绝，因不利于自动化引用和回归追踪。

---

### D6: 三级回归阈值

**决策:**

| 退化幅度 | 动作 |
|---------|------|
| <10% | 记录，不拦截 |
| 10%~20% | 拦截 PR，需 ADR 说明原因 |
| >20% | 拦截 PR，升级为事故复盘 |

CoCond 阈值放宽至 30% (因其高变异性)。co_switch 延迟阈值 50% (纳秒级波动大)。

**理由:**
- 10% 以内的波动在正常测试噪声范围内
- 10~20% 需要解释但可能是有意取舍 (如 MLFQ 的 -3.3%)
- >20% 几乎肯定是回归
- CoCond 的 Wait/NotifyAll 比率受调度随机性影响大，需放宽

**替代方案:** 固定 10% — 拒绝，因 CoCond 和 co_switch 延迟会被误拦。

---

### D7: fatigue_metrics.h 作为埋点单一事实源

**决策:** 所有压测程序共享 `benchmark_test/fatigue_metrics.h`，原子计数器 + 周期 JSON 输出由统一实现提供。

**理由:**
- 避免各模块独立实现导致 JSON 格式不一致
- 新增字段只需修改一处
- 已有成熟实现 (PR #199 引入的 Metrics struct + reporter 线程)

**替代方案:** 各模块独立实现埋点 — 拒绝，因历史遗留问题 (不同格式的日志输出)。

---

## 后果

### 正向
- 测试结果可机器解析、可自动对比
- 基线管理消除 "看起来慢了" 的主观判断
- Trap-ID 使可靠性验证可追溯

### 负面
- JSON 输出 ~1KB/帧/模块，全模块 + 高频采样可能产生大量日志
- 需要维护 baseline 对比脚本 (record_baseline.py)
- 新字段需同步 fatigue_metrics.h + Python parser

### 风险缓解
- FATIGUE_INTERVAL 默认 60s → ~1KB/min/模块，1h ≈ 60KB (可忽略)
- record_baseline.py 已实现，按 JSON 名称过滤避免 parser 不同步
