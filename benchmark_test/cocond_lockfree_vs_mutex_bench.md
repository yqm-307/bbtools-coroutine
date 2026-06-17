# CoCond lockfree → std::queue+mutex 性能对照

> 2026-06-18 | PR #205 | CoCond 异常安全改造

## 背景

PR #205 为修复 CoCond::Wait() 异常安全问题，将 waiter 队列从 `boost::lockfree::queue<CoWaiter*>` 改为 `std::queue<std::shared_ptr<CoWaiter>>` + `std::mutex`。

为评估 lockfree → mutex 的性能代价，编写了独立压测 `cocond_queue_stress.cc`，去除了 `unified_stress` 中 `bbtco_sleep(500)` 的限流瓶颈。

## 测试条件

- 二进制: `cocond_queue_stress`
- 200 waiter 协程, 1 notifier
- notifier 用 `bbtco_yield` 极限频率 `NotifyAll()`
- waiter 无 yield/sleep, `Wait()` → ops++ → 立即重入 `Wait()`
- `CO_PROC_THREADS=8`, dur=10s, 各 3 轮

## 结果

| 版本 | R1 | R2 | R3 | **平均** |
|------|-----|-----|-----|----------|
| PRE lockfree (28a8f3c) | 8,723 | 8,518 | 8,666 | **8,636 ops/s** |
| POST mutex (50dd685)  | 8,350 | 8,194 | 8,310 | **8,285 ops/s** |

**Delta: -4.1%**

## 为什么 unified_stress 测不出

`stress_cocond()` 中 notifier 每 500ms 一次 `NotifyAll()`，理论天花板 400 ops/s。队列操作（微秒级）在 500ms 间隔中占比可忽略，两个版本都 ~388 ops/s（噪声级别）。去掉限流后，200 waiter 同时 `Wait()` push + `NotifyAll()` pop 时的 mutex 竞争才暴露真实差距。

## 结论

-4.1% 可接受。CoCond 是通知型同步，Wait/NotifyAll 间有协程调度间隙，mutex 开销温和（对比 MLFQ 的 -8.6% 纯锁热路径）。换来的异常安全（消除悬空指针 UB）值得。
