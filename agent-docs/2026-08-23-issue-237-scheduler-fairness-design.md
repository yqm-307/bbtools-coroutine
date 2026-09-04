# Issue #237：单 Processer 调度公平性修复设计

- 日期：2026-08-23
- 状态：已落地（PR #239）
- 适用仓库：`bbtools-coroutine`
- 关联 Issue：[Issue #237](https://github.com/yqm-307/bbtools-coroutine/issues/237)
- ARCH_RISK: MEDIUM

## 1. 背景与证据

`unified_stress` 在全模块、单 Processer（`--threads=1`）下，`CoCond` waiter 在首次唤醒后停止恢复。已在当前 `main` 定向构建并复现：

```bash
FATIGUE_INTERVAL=10 ./build/bin/benchmark_test/unified_stress --threads=1 30 0 0
```

30.1 秒时 `cocond` 指标为 `cond_waits=200`、`cond_signals=58`、`errors=9400`。同一二进制下：

- 仅运行 `--module=cocond --threads=1`，`cond_waits` 持续增长且 `errors=0`；
- 全模块 `--threads=4`，16.2 秒时 `cond_waits=5968`、`errors=0`。

因此，问题是全模块负载下单 Processer 的调度公平性问题，不是 `CoCond` 独立功能失效。

## 2. 根因判断

已确认的调度规则：

- `Coroutine::OnCoPollEvent()` 将 timeout 事件提升为 `CO_PRIORITY_CRITICAL`（`bbt/coroutine/detail/Coroutine.cc`）；
- `Processer::_Run()` 对 CRITICAL 队列设置无限执行配额（`bbt/coroutine/detail/Processer.cc`）；
- `CoCond` 的 custom event 未提升优先级，保持 NORMAL；
- 全模块压测中多个模块持续执行 `bbtco_sleep(1)`，不断产生 timeout 唤醒。

这会使单 Processer 持续清空 CRITICAL 队列，NORMAL 的 `CoCond` waiter 无法得到执行机会。多 Processer 可由其他工作线程消费 NORMAL 队列，因此不复现。

Issue 原先提出的 `m_onyield_callback` 覆盖解释不作为修复依据：callback 完成后，`Coroutine::CommitYield()` 才调用 `CoPollEvent::CommitPark()` 提交事件等待。该时序需要保留，不在本次修改范围。

## 3. 目标与非目标

### 3.1 目标

1. 保持 timeout 事件的 CRITICAL 优先级。
2. 单 Processer 在持续 timeout 压力下，NORMAL 事件不发生无限饥饿。
3. 在各优先级持续有任务时，以实际 `Resume()` 运行时间按比例分配服务份额。
4. 建立确定性回归测试，验证 NORMAL waiter 在固定 deadline 内持续被调度。
5. 通过全模块单 Processer 压测验证 `CoCond` 的 `errors=0` 且 `cond_waits` 持续增长。

### 3.2 非目标

- 不修改 `Context`、`CoWaiter`、`CoPollEvent` 或 `CoCond` 的事件注册/提交时序；
- 不将 timeout 降为 HIGH 或 NORMAL；
- 不通过修改 `unified_stress` 规避问题；
- 不引入 age-based scheduler、双队列或新的公开调度 API；
- 不改变多 Processer 的负载均衡和 work-steal 策略。

## 4. 方案：优先级运行时间预算

采用 Processer 内部的 work-conserving weighted round-robin。它借鉴 Linux 对高优先级工作施加 runtime bandwidth 的边界，而不是将 CFS/EEVDF 的每协程虚拟运行时间树直接迁入本库。

每个公平轮次向四个优先级发放固定运行时间预算：

| 优先级 | 每轮预算 | 目的 |
|---|---:|---|
| CRITICAL | 600 µs | timeout 仍优先，并获得最大服务份额 |
| HIGH | 200 µs | 保持高于普通任务的服务份额 |
| NORMAL | 150 µs | 在持续 CRITICAL 压力下获得明确进展保证 |
| LOW | 50 µs | 避免低优先级永久饥饿 |

单轮总预算为 1000 µs。该数字是调度记账轮次，不是硬实时 wall-clock 周期：用户协程的单次 `Resume()` 不可被用户态调度器中断，可能使实际轮次时长超过 1000 µs。

### 4.1 选择与扣账

1. Processer 从有剩余预算的非空队列中，选择最高优先级任务；
2. 执行一次 `Coroutine::Resume()`；
3. 使用现有 `m_last_run_us` 扣减该优先级余额，单次最少扣减 1 µs；
4. 某优先级余额耗尽后，若其他非空优先级仍有余额，必须转向后者；
5. 当所有非空队列都无余额时，重置所有优先级余额，进入下一公平轮次；
6. 若仅有已耗尽的队列非空，立即开始下一轮次，保证 CPU 不因预算空转；新到达的低优先级任务最迟在当前高优先级余额耗尽后取得本轮预算。

```text
timeout event
  └─ CRITICAL queue：先运行，扣减 CRITICAL budget
      └─ CRITICAL budget 耗尽且 NORMAL runnable
          └─ NORMAL queue：获得至少 150 µs 的本轮服务机会
```

因此，本方案保证的是「持续 runnable 的 NORMAL 队列不会无限饥饿」，而不是将 timeout 降级或承诺不可抢占协程的硬实时延迟上界。

### 4.2 与成熟运行时的关系

- [Linux CFS](https://docs.kernel.org/scheduler/sched-design-CFS.html) 与 [EEVDF](https://docs.kernel.org/scheduler/sched-eevdf.html) 对每个 task 做虚拟运行时间记账；本方案仅在优先级类别层面做实际运行时间记账，避免引入每协程有序树和跨 Processer `vruntime` 同步。
- [Linux RT bandwidth](https://docs.kernel.org/scheduler/sched-rt-group.html) 限制高优先级 runtime 为其他任务保留 CPU；本方案采用相同的「优先级不变、带宽有界」原则。
- [Tokio cooperative scheduling](https://docs.rs/tokio/latest/tokio/task/coop/index.html) 用 task budget 防止单 task 长期独占；本库已有主动 Yield 与 MLFQ，本方案补足优先级队列之间的公平性。
- [Go runtime](https://go.dev/src/runtime/proc.go) 周期性检查全局 runnable queue；本库已有全局队列和 work-steal，本方案解决同一 Processer 内优先级队列的饥饿。

### 4.3 禁止项

- 不在 callback 中新增锁或跨协程同步；
- 不调整 `CommitPark()` 的调用位置；
- 不为 `CoCond` 单独提升优先级；
- 不以 sleep、重试或更长 timeout 掩盖饥饿；
- 不引入每协程 `vruntime`、红黑树或新的公开 Scheduler API；
- 不改变 Issue #237 以外的模块语义。

## 5. 测试设计

### 5.1 定向回归测试

在已有 `Test_copollevent_state` 的独立 Scheduler 生命周期框架中新增用例：

1. 保存并暂时设置 `m_cfg_static_thread_num=1`；
2. 启动 Scheduler；
3. 创建 `CoCond` waiter 和 notifier，确认 waiter 已完成首轮等待；
4. 并发启动多个 `bbtco_sleep(1)` 循环，持续制造 CRITICAL timeout 压力；
5. 连续触发 `NotifyAll()`，断言 NORMAL waiter 计数在固定 deadline 内继续增长；
6. 确认压力协程持续产生 CRITICAL timeout，以证明 NORMAL 进展不是因压力提前结束；
7. 停止 Scheduler 并恢复原始配置。

该测试必须在修复前失败、修复后通过。它验证调度公平性，不依赖 `unified_stress` 的错误阈值或 wall-clock 采样顺序。

### 5.2 回归验证

实现后依次执行：

```bash
cmake --build build --target Test_copollevent_state unified_stress --parallel
ctest --test-dir build -R '^Test_copollevent_state$' --output-on-failure
ctest --test-dir build --output-on-failure
FATIGUE_INTERVAL=10 ./build/bin/benchmark_test/unified_stress --threads=1 30 0 0
FATIGUE_INTERVAL=10 ./build/bin/benchmark_test/unified_stress --threads=4 15 0 0
```

通过条件：

- 新回归用例在 deadline 内完成；
- 全量 CTest 通过，无新增编译器警告；
- 单 Processer 全模块 30 秒中，`cocond.errors=0`，`cond_waits` 随 `cond_signals` 持续增长；
- 多 Processer 对照同样无 `cocond` 错误；
- `git diff` 仅包含调度预算、定向测试和必要的 CMake 注册变更。

## 6. 风险与回滚

风险：CRITICAL timeout 的批处理尾延迟可能增加，且核心 Processer 的改动会影响所有协程事件。单次不主动 Yield 的协程可超过预算；这是 cooperative 模型的固有限制。

缓解：保留 timeout 的 CRITICAL 身份和最大服务份额；只在 `Resume()` 返回后扣账；对极短运行至少扣 1 µs；用单 Processer 高压回归、单/多 Processer `unified_stress` 和全量 CTest 覆盖。

回滚：恢复 CRITICAL 无限制优先策略即可回到当前行为。回滚会重新引入 NORMAL 饥饿，因此仅用于确认出现不可接受的 timeout 延迟回归时。

## 7. 设计一致性检查

- 数据格式：不新增状态、配置字段或公共数据格式；
- 时序：保持「事件注册 callback 完成，再 `CommitPark()`」的现有时序；
- 参数传递：只在 Processer 内部维护优先级运行时间预算；
- 禁止项：未改变 timeout 优先级、未修改 `CoCond` 和 `Context`。

结论：consistent。
