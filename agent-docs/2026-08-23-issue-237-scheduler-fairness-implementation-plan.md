# Issue #237：调度公平性实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans` 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法跟踪进度。

**目标：** 在单 Processer 的持续 timeout 压力下，为 NORMAL runnable coroutine 提供运行时间进展保证，并保持 timeout 的 CRITICAL 优先级。

**架构：** `Processer` 保留四个既有优先级队列，但将无限 CRITICAL 批次替换为 Processer 局部运行时间预算。一个公平轮次的预算为 LOW / NORMAL / HIGH / CRITICAL = 50 / 150 / 200 / 600 µs。每次 `Resume()` 返回后使用已有 `Coroutine::GetLastRunTimeUs()` 扣减对应余额，单次至少扣减 1 µs。只有所有非空队列均无余额时才重置轮次；没有其他可运行队列时立即重置，保持 work-conserving。

**技术栈：** C++17、Boost.Test、CMake、现有 `Coroutine` / `Processer` / `Scheduler` 实现。

**设计规格：** `agent-docs/2026-08-23-issue-237-scheduler-fairness-design.md`

---

## 文件结构

- 修改：`bbt/coroutine/detail/Processer.cc`
  - 在 `_Run()` 中维护局部优先级运行时间余额；按剩余余额选择队列，在一次协程恢复后扣账；保留全局取任务、work-steal、关闭回收和 MLFQ 数据采集。
- 修改：`unit_test/Test_copollevent_state.cc`
  - 在独立 Scheduler 生命周期下加入单 Processer 压力回归：持续 CRITICAL timeout 不能阻断 NORMAL `CoCond` waiter 的进展。
- 不修改：`Processer.hpp`、`GlobalConfig.hpp`、`CMakeLists.txt`
  - 本期预算是 `Processer.cc` 的内部常量，不新增公开配置或 API；`Test_copollevent_state` 已有 CMake target 和 30 秒 CTest timeout。

## 任务 1：先写单 Processer 饥饿回归

**文件：**
- 修改：`unit_test/Test_copollevent_state.cc`，`t_multi_processer_yield_requeues_each_coroutine_once_per_iteration` 之前
- 测试：`unit_test/Test_copollevent_state.cc`

- [ ] **步骤 1：加入失败的回归用例**

在 `Test_copollevent_state.cc` 的匿名命名空间中保留现有 `WaitForAtLeast`、`StaticProcesserCountRestore` 和 `SchedulerStopGuard`，并在末尾新增以下用例。该用例先让 8 个 waiter 完成 warm-up，再启动 128 个 `bbtco_sleep(1)` 压力协程；notifier 本身也以 1 ms timeout 驱动。旧实现的无限 CRITICAL 配额会令 `post_pressure_wakeups` 停止增长。

```cpp
BOOST_AUTO_TEST_CASE(t_single_processer_timeout_pressure_does_not_starve_normal_cocond)
{
    constexpr int kWaiterCount = 8;
    constexpr int kPressureCoroutineCount = 128;
    constexpr int kWarmupWakeups = kWaiterCount;
    constexpr int kRequiredPostPressureWakeups = kWaiterCount * 2;

    std::atomic_bool running{true};
    std::atomic_bool pressure_enabled{false};
    std::atomic_int warmup_wakeups{0};
    std::atomic_int post_pressure_wakeups{0};
    std::atomic_int pressure_ticks{0};

    auto& config = *g_bbt_coroutine_config;
    StaticProcesserCountRestore restore_processer_count{config};
    config.m_cfg_static_thread_num = 1;

    auto& scheduler = g_scheduler;
    SchedulerStopGuard scheduler_stop{*scheduler};
    scheduler->Start();
    scheduler_stop.MarkStarted();

    auto cond = sync::CoCond::Create();
    BOOST_REQUIRE(cond != nullptr);

    for (int i = 0; i < kWaiterCount; ++i)
    {
        bool registered = false;
        scheduler->RegistCoroutineTask([cond,
                                        &running,
                                        &pressure_enabled,
                                        &warmup_wakeups,
                                        &post_pressure_wakeups] {
            while (running.load(std::memory_order_acquire))
            {
                cond->Wait();
                if (!running.load(std::memory_order_acquire))
                    break;

                if (pressure_enabled.load(std::memory_order_acquire))
                    post_pressure_wakeups.fetch_add(1, std::memory_order_release);
                else
                    warmup_wakeups.fetch_add(1, std::memory_order_release);
            }
        }, registered);
        BOOST_REQUIRE(registered);
    }

    bool notifier_registered = false;
    scheduler->RegistCoroutineTask([cond, &running] {
        while (running.load(std::memory_order_acquire))
        {
            cond->NotifyAll();
            bbtco_sleep(1);
        }
    }, notifier_registered);
    BOOST_REQUIRE(notifier_registered);

    BOOST_REQUIRE(WaitForAtLeast(warmup_wakeups,
                                 kWarmupWakeups,
                                 std::chrono::seconds(3)));

    pressure_enabled.store(true, std::memory_order_release);
    for (int i = 0; i < kPressureCoroutineCount; ++i)
    {
        bool registered = false;
        scheduler->RegistCoroutineTask([&running, &pressure_ticks] {
            while (running.load(std::memory_order_acquire))
            {
                pressure_ticks.fetch_add(1, std::memory_order_release);
                bbtco_sleep(1);
            }
        }, registered);
        BOOST_REQUIRE(registered);
    }

    BOOST_REQUIRE(WaitForAtLeast(pressure_ticks,
                                 kPressureCoroutineCount * 4,
                                 std::chrono::seconds(3)));
    BOOST_CHECK(WaitForAtLeast(post_pressure_wakeups,
                               kRequiredPostPressureWakeups,
                               std::chrono::seconds(3)));

    running.store(false, std::memory_order_release);
    scheduler_stop.Stop();
    restore_processer_count.Restore();
}
```

- [ ] **步骤 2：构建并确认旧实现失败**

运行：

```bash
cmake --build build --target Test_copollevent_state --parallel
ctest --test-dir build -R '^Test_copollevent_state$' --output-on-failure
```

预期：构建成功；CTest 失败，失败断言来自 `post_pressure_wakeups` 无法在 3 秒 deadline 内达到 `kRequiredPostPressureWakeups`。若用例未失败，不修改生产代码；增加压力协程数量或缩短 notifier 周期，直到旧实现稳定暴露饥饿。

## 任务 2：实现 Processer 运行时间预算

**文件：**
- 修改：`bbt/coroutine/detail/Processer.cc:107-188`
- 测试：`unit_test/Test_copollevent_state.cc`

- [ ] **步骤 1：在 `_Run()` 初始化局部余额**

在 `_Run()` 的 `while` 之前，替换当前 `kPriorityQuota` 思路为以下局部常量和余额。数组顺序必须匹配 `CoroutinePriority` 枚举：LOW、NORMAL、HIGH、CRITICAL。

```cpp
static constexpr std::array<uint64_t, CO_PRIORITY_COUNT> kPriorityRuntimeBudgetUs = {
    50,   // LOW
    150,  // NORMAL
    200,  // HIGH
    600,  // CRITICAL
};

auto priority_runtime_budget_us = kPriorityRuntimeBudgetUs;
```

在 `Processer.cc` 顶部新增 `#include <array>` 和 `#include <algorithm>`。

- [ ] **步骤 2：按余额执行并在 `Resume()` 后扣账**

将现有「每优先级固定 task 数量」的嵌套循环替换为以下规则：

```cpp
bool any_dequeued = false;
for (auto&& p : {CO_PRIORITY_CRITICAL,
                 CO_PRIORITY_HIGH,
                 CO_PRIORITY_NORMAL,
                 CO_PRIORITY_LOW})
{
    while (priority_runtime_budget_us[p] > 0)
    {
        if (!m_coroutine_queue[p].try_dequeue(m_running_coroutine) ||
            m_running_coroutine == nullptr)
            break;

        any_dequeued = true;
        if (m_is_shutdown.load(std::memory_order_acquire))
        {
            delete m_running_coroutine;
            m_running_coroutine = nullptr;
            continue;
        }

        AssertWithInfo(m_running_coroutine->GetStatus() != CO_RUNNING &&
                           m_running_coroutine->GetStatus() != CO_FINAL,
                       "bad coroutine status!");

        m_running_coroutine_begin.exchange(bbt::core::clock::gettime_mono<>());
#ifdef BBT_COROUTINE_PROFILE
        m_co_swap_times++;
#endif
        m_running_coroutine->Resume();
        m_running_coroutine->SetLastRunTimeUs(
            bbt::core::clock::gettime_mono<>() - m_running_coroutine_begin.load());

        const auto charged_us = std::max<uint64_t>(
            1, m_running_coroutine->GetLastRunTimeUs());
        priority_runtime_budget_us[p] =
            charged_us >= priority_runtime_budget_us[p]
                ? 0
                : priority_runtime_budget_us[p] - charged_us;

        const auto disposition = m_running_coroutine->CommitYield();
        if (disposition == CoroutineYieldDisposition::READY)
            g_scheduler->OnActiveCoroutine(CO_PRIORITY_NORMAL, m_running_coroutine);
        else if (disposition == CoroutineYieldDisposition::FINAL)
            delete m_running_coroutine;
        m_running_coroutine = nullptr;
    }
}
```

不要改变 `READY` 回投 NORMAL 的既有行为。不要对 shutdown 路径扣账，因为该路径没有执行用户协程。

- [ ] **步骤 3：实现 work-conserving 轮次重置**

在每次外层循环结束前，按本地优先级队列的 `size_approx()` 计算 runnable 队列：空队列不参与预算耗尽判定；当没有本地 runnable 队列，或所有非空队列余额均为 0 时，恢复全部预算。恢复后不得提前 `continue`，必须继续经过全局取数、work-steal 和 `wait_for` 休眠路径，避免 `size_approx` 假阳性形成忙循环。

```cpp
bool has_runnable_queue = false;
bool has_runnable_queue_with_budget = false;
for (auto&& p : {CO_PRIORITY_CRITICAL,
                 CO_PRIORITY_HIGH,
                 CO_PRIORITY_NORMAL,
                 CO_PRIORITY_LOW})
{
    if (m_coroutine_queue[p].size_approx() == 0)
        continue;
    has_runnable_queue = true;
    if (priority_runtime_budget_us[p] > 0)
        has_runnable_queue_with_budget = true;
}
if (!has_runnable_queue || !has_runnable_queue_with_budget)
    priority_runtime_budget_us = kPriorityRuntimeBudgetUs;
```

shutdown 路径在预算循环前直接回收各优先级本地队列，避免关闭时受预算门控。此处只重置余额或回收队列；不可改变正常路径的队列顺序。

- [ ] **步骤 4：编译并确认定向回归通过**

运行：

```bash
cmake --build build --target Test_copollevent_state --parallel
ctest --test-dir build -R '^Test_copollevent_state$' --output-on-failure
```

预期：构建成功；`Test_copollevent_state` 通过，新增用例在 3 秒内观察到 timeout 压力和持续增长的 NORMAL waiter 计数。

## 任务 3：执行回归与单/多 Processer 压测

**文件：**
- 修改：无
- 测试：已修改的 `Processer.cc` 与 `Test_copollevent_state.cc`

- [ ] **步骤 1：运行全量 CTest**

运行：

```bash
ctest --test-dir build --output-on-failure
```

预期：所有已注册 CTest 通过；无 timeout、断言、crash 或新增编译器警告。

- [ ] **步骤 2：复跑 #237 原始单 Processer 复现**

运行：

```bash
FATIGUE_INTERVAL=10 ./build/bin/benchmark_test/unified_stress --threads=1 30 0 0
```

预期：退出码为 0；最后一条 `name":"cocond"` 指标的 `errors=0`，且 `cond_waits` 随 `cond_signals` 持续增长；stdout 不含 `[cocond] WARN:`。

- [ ] **步骤 3：运行多 Processer 对照**

运行：

```bash
FATIGUE_INTERVAL=10 ./build/bin/benchmark_test/unified_stress --threads=4 15 0 0
```

预期：退出码为 0；`cocond.errors=0`，且 `cond_waits` 持续增长，确认新预算不破坏多 Processer 场景。

- [ ] **步骤 4：审阅最终差异，不创建提交**

运行：

```bash
git diff --check
git diff -- bbt/coroutine/detail/Processer.cc unit_test/Test_copollevent_state.cc agent-docs/
git status --short
```

预期：无空白错误；仅包含设计文档、实现计划、`Processer.cc` 和 `Test_copollevent_state.cc` 的必要修改。除非用户明确授权，不执行 `git commit`、`git push`、Issue 评论或关闭操作。
