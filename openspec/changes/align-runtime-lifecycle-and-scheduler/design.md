## 背景

阶段 1 的 `extract-runtime-wait-protocol` 解决的是等待语义与桥接稳定面，但 runtime 内核本身仍然存在两类没有收口的问题：

- 调度控制路径的并发边界仍然含糊
- 协程对象、事件对象和 runnable 队列的所有权与 teardown 路径仍然含糊

如果这两类问题不在阶段 2 被单独处理，后续 `sync`、Hook 和 `CoPool` 的迁移只会把不稳定性包裹得更深，而不会真正消除它。

## 当前代码证据

### 1. 调度状态与负载均衡状态缺少清晰的并发语义

从 `Scheduler` 与 `Processer` 的实现看，当前调度控制路径里至少有三类共享状态没有被明确建模：

- `Scheduler` 的 `m_is_running`、`m_run_status` 使用 `volatile` 声明，但这些字段跨线程参与启动和停止流程，`volatile` 不能提供 C++ memory model 下的同步保证。
- `Scheduler::_LoadBlance2Proc()` 直接递增 `m_load_idx`，`Scheduler::TryWorkSteal()` 直接递增 `m_steal_idx`，但这些索引会在多 worker 场景下被并发访问。
- `Processer::AddCoroutineTask()` 用 `m_run_status == PROC_SUSPEND` 与 `m_run_cond_notify.compare_exchange_strong()` 组合决定是否唤醒 worker，但 stop、steal 和本地 runnable 消费会同时影响这条路径。

这些实现细节意味着当前调度核心虽然“通常能工作”，但还没有形成一个规格化的、可证明的数据竞争边界。

### 2. runnable 队列与协程对象的所有权模型没有真正冻结

`Coroutine` 当前通过 `typedef Coroutine* Ptr;` 使用裸指针，但注释仍在讨论基于 `shared_ptr` 的所有权模型。这暴露出实现与设计心智模型已经发生偏移。

更关键的是，当前不同路径对协程对象的结束处理并不统一：

- `Scheduler::RegistCoroutineTask()` 创建裸指针协程对象，并将其投递给某个 `Processer` 或全局队列。
- `Processer::_Run()` 只在协程进入 `CO_FINAL` 时 `delete m_running_coroutine`。
- `Scheduler::Stop()` 与 `Processer::Stop()` 在清空全局或本地 runnable 队列时只是反复取出指针并置空，没有把“残留协程如何终结或释放”做成显式语义。

这说明 `coroutine-lifecycle` spec 里的“单一所有权模型”和“teardown 时残留协程必须被安全终结或显式保留”在当前实现中尚未落地。

### 3. stop 协议目前更多是实现凑合，而不是统一语义

`Scheduler::Stop()` 当前做的事情大致是：

1. `m_is_running = false`
2. 销毁所有 `Processer`
3. join scheduler 线程
4. 清空全局 runnable 队列

`Processer::Stop()` 则通过循环设置 `m_is_running = false`、睡眠 50ms 并 `notify_one()`，一直等到 `PROC_EXIT`。

这种实现方式的主要问题不是“写得不好看”，而是：

- stop 的所有参与方没有统一的阶段语义，例如请求停止、唤醒阻塞 worker、停止接受新 runnable、处理残留协程、最终退出。
- scheduler、processer、coroutine teardown 之间没有一个单一的终结协议，导致 residual runnable 的归属仍然模糊。
- 现有实现依赖轮询和局部条件变量通知，虽然大概率能退出，但并没有把“确定性 stop”做成可验证契约。

### 4. 生命周期测试素材存在，但离阶段 2 的验证目标还差一层

当前仓库里已经有一些可复用素材：

- `Test_coroutine.cc` 覆盖了最基础的 `Resume()` / `Yield()` 往返。
- `Test_coroutine_exception.cc` 覆盖了用户协程异常被路由到异常处理器，以及协程进入 `CO_FINAL`。
- `Test_coroutine_stack.cc` 覆盖了栈对象创建压力。

但这些测试还不能证明阶段 2 已完成，因为它们尚未直接覆盖：

- runtime stop 时全局/本地 runnable 队列残留对象如何终结
- 多 worker 并发下的负载均衡和 steal 状态边界
- `YieldWithCallback()` 顺序保证是否在 runtime 重构后仍然成立
- 事件注册、异常和 teardown 交叠时的生命周期一致性

## 目标 / 非目标

**目标：**

- 收敛 `Scheduler` 与 `Processer` 的职责边界，使调度、窃取、唤醒与 stop 路径具有明确的并发语义。
- 收敛 `Coroutine` 与 `Context` 的所有权和状态迁移，使创建、执行、挂起、重新激活、完成与 teardown 拥有单一生命周期模型。
- 明确 coroutine / event / runnable 队列之间的对象归属与终结规则。
- 在不修改阶段 1 等待协议语义的前提下，让 runtime 核心成为后续 `sync` 迁移可依赖的稳定地基。
- 为阶段 2 建立文件级测试切分，覆盖调度、生命周期、异常、teardown 与压力场景。

**非目标：**

- 本 change 不直接重写 `CoCond`、`CoMutex`、`Chan` 或 `CoRWMutex`。
- 本 change 不直接收口 Hook、`CoPool`、`coroutine.hpp` 或 `syntax/*` 的最终公共行为。
- 本 change 不重新设计阶段 1 的桥接稳定面。
- 本 change 不要求此时就暴露新的 public API。

## 设计决策

### 1. 先冻结生命周期与 stop 协议，再讨论底层指针封装形式

阶段 2 的首要目标不是先决定 `Coroutine` 到底用裸指针、`unique_ptr` 还是别的封装，而是先明确：

- 谁拥有 runnable 状态下的协程对象
- 谁有权把协程迁移到终态
- stop 时残留协程由谁终结
- worker 退出前需要清理什么

决策理由：

- 现阶段真正不稳定的是所有权规则，而不是语法层面的指针包装。
- 如果规则没有冻结，先替换指针类型只会掩盖问题而不是解决问题。

### 2. Scheduler 与 Processer 的共享状态必须收敛到可证明的同步边界

后续实现应把 `m_is_running`、`m_run_status`、负载均衡索引、steal 索引以及 stop/notify 相关状态统一纳入明确的同步语义，而不是继续依赖 `volatile`、隐式原子性或“多数平台上看起来没问题”的假设。

决策理由：

- `runtime-scheduler` spec 已经要求并发安全的状态迁移。
- 阶段 2 若不先清掉这些共享状态风险，后续阶段的任何性能或行为测试都可能建立在未定义行为之上。

### 3. stop 协议应拆成显式阶段，而不是分散在多个函数里凑出来

建议将 stop 语义至少拆为以下逻辑阶段：

1. 请求停止并阻止新执行循环继续扩张
2. 唤醒处于空闲/挂起状态的 worker
3. 终结或转移残留 runnable 状态
4. 等待 worker 与 scheduler 线程退出
5. 进入文档化的终态

决策理由：

- 这样才能把 `Scheduler::Stop()`、`Processer::Stop()`、runnable 队列清理和 coroutine teardown 绑定成同一协议。
- 也更方便建立针对 idle stop、residual runnable stop 和 repeated start-stop 的测试。

### 4. Yield-with-callback 的顺序保证属于阶段 2 的核心回归面

阶段 1 已经冻结了等待桥接面，但真正保证“先挂起、再注册事件”的 still-critical path 仍然落在 `Context::YieldWithCallback()` 与 `Coroutine::YieldWithCallback()`。

因此阶段 2 必须把以下语义视为硬约束：

- 协程在进入 suspended 之前，不能被新事件重新激活
- callback 失败时的回退路径必须保持状态一致
- runtime 核心重构后，不能破坏阶段 1 bridge 依赖的注册顺序保证

### 5. 阶段 2 只消费阶段 1 稳定面，不反向改写它

本阶段若发现需要修改 custom wait、fd wait、timeout 完成模型或 bridge surface，本质上应回退判定为阶段 1 未完成，而不是在阶段 2 中继续扩展协议范围。

决策理由：

- 阶段 2 的职责是收敛 runtime 内核，不是再次扩张协议面。
- 只有保持这个边界，后续 `sync` 迁移才能真正建立在稳定前提上。

## 建议测试文件拆分

参考当前 `unit_test/` 的组织方式，阶段 2 更适合新增以下测试文件：

| 建议文件 | 主要职责 |
|----------|----------|
| `unit_test/Test_scheduler_shutdown.cc` | 覆盖 idle stop、residual runnable stop、worker 唤醒与 teardown 顺序 |
| `unit_test/Test_scheduler_concurrency.cc` | 覆盖 load-balance、work-steal、并发投递与 stop 边界 |
| `unit_test/Test_coroutine_lifecycle_state.cc` | 覆盖 runnable/running/suspended/final 合法迁移与非法迁移诊断 |
| `unit_test/Test_coroutine_lifecycle_teardown.cc` | 覆盖残留 coroutine 终结、event teardown、yield-with-callback 顺序回归 |
| `unit_test/Test_coroutine_lifecycle_stress.cc` | 覆盖 repeated start-stop、频繁 resume/yield、异常与 teardown 交叠压力 |

### 建议 test case 粒度

`Test_scheduler_shutdown.cc`

- `t_scheduler_stop_wakes_idle_processers`
- `t_scheduler_stop_handles_residual_global_runnable`
- `t_processer_stop_exits_without_busy_wait_leak`

`Test_scheduler_concurrency.cc`

- `t_load_balance_state_is_data_race_free`
- `t_work_steal_does_not_duplicate_coroutines`
- `t_scheduler_stop_blocks_new_runnable_expansion`

`Test_coroutine_lifecycle_state.cc`

- `t_coroutine_state_transition_resume_yield`
- `t_coroutine_suspend_on_awaitable_event`
- `t_illegal_state_transition_is_observable`

`Test_coroutine_lifecycle_teardown.cc`

- `t_residual_coroutine_has_defined_owner_during_teardown`
- `t_yield_with_callback_preserves_registration_order`
- `t_exception_path_cleans_active_await_event`

`Test_coroutine_lifecycle_stress.cc`

- `t_repeated_start_stop_does_not_leave_residual_coroutines`
- `t_resume_yield_stress_preserves_finalization`
- `t_exception_and_teardown_interleaving_remains_stable`

## 迁移计划

1. 先以阶段 1 的桥接稳定面为前提，梳理 `Scheduler`、`Processer`、`Coroutine`、`Context` 的共享状态与所有权边界。
2. 先冻结 stop 协议和生命周期规则，再决定具体实现收敛方式。
3. 优先补 scheduler shutdown / concurrency 与 coroutine lifecycle / teardown 的文件级测试出口。
4. 当阶段 2 的调度与生命周期稳定面成立后，再进入 `migrate-sync-primitives-to-wait-protocol`。

## 风险 / 权衡

- [风险] 如果阶段 2 试图顺手修复阶段 1 协议问题，会导致阶段边界再次膨胀 → 缓解方式：明确任何基础等待语义变化都视为阶段 1 回退。
- [风险] 若先改指针封装再改 stop 协议，可能只会掩盖所有权问题 → 缓解方式：先冻结生命周期规则，再讨论封装形式。
- [风险] 现有测试对 teardown 和并发边界覆盖不足，容易误判“改完能跑”就等于稳定 → 缓解方式：把阶段 2 的新增测试文件作为正式出口，而不是附加优化项。