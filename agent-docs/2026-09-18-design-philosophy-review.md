# bbtools-coroutine 协程设计哲学系统性评估

**状态：** 调研评估文档（2026-09-18），基于 `main`（`c934189`）的代码与契约文档
**契约真源：** `agent-docs/2026-09-07-core-runtime-contract.md`（下称「契约 §N」）
**证据口径：** 所有技术论断均标注仓内文件路径与行号；无法从本仓代码确认的论断显式标注「无法验证」。`bbt::pollevent::EventLoop`（ASIO backend）位于外部依赖 `bbt_core`（`CMakeLists.txt:83` 链接 `bbt_core`），其内部实现不在本仓，凡涉及 backend 内部行为一律按不可验证处理。

## 0. 背景论断复核

任务背景中的论断逐条对照代码复核结果：

| 背景论断 | 复核结论 |
|---|---|
| 独立调度线程 `m_sche_thread` 跑 `_Run`，与 worker 分离 | **属实**。`Scheduler.cc:264-268` 创建调度线程执行 `_Run`；主循环在 `Scheduler.cc:245-251` |
| 协作式、不抢占；长任务独占 worker | **属实**。代码中无任何抢占机制；契约 §2 显式声明「不承诺 Go runtime 的抢占或自动恢复能力」 |
| 定时器/事件走 `CoPollEvent` → 底层 EventLoop → `Trigger` → `OnCoPollEvent` → 入全局队列 → Processer 恢复 | **属实**。路径见 `CoPollEvent.cc:90-131`（Trigger→`_Complete`）、`Coroutine.cc:440-479`（`OnCoPollEvent`→`OnActiveCoroutine`）、`Scheduler.cc:102-133`（入 `m_global_coroutine_queue`） |
| `_FixTimingScan` + `m_cfg_worker_stall_warn_ms` 停顿检测 | **属实**。`Scheduler.cc:135-201`；配置项 `GlobalConfig.hpp:76-78`，默认 0=关闭 |
| 1ms 粒度、亚毫秒向上取整 | **属实**。`YieldUntilTimeout(int ms)`（`Coroutine.hpp:168`）；`Hook.cc:151` `(tv_usec+999)/1000`；`Hook.cc:705` `(usec+999)/1000`；`agent-docs/2026-09-07-m1-04b-hook-timeout-flags-gap.md` 明示「超时 1ms 粒度，亚毫秒向上取整」 |

## 1. 架构与线程模型

### 1.1 三类执行角色

| 角色 | 线程绑定 | 职责 | 代码依据 |
|---|---|---|---|
| Scheduler 线程 | `m_sche_thread`，`Start(SCHE_START_OPT_SCHE_THREAD)` 创建 | 事件轮询（`PollOnce`）、worker 停顿扫描（`_FixTimingScan`）、栈池维护（`StackPool::OnUpdate`） | `Scheduler.cc:222-224`、`264-268`、`245-251` |
| Processer（worker） | `thread_local`，每线程一个 | 执行协程：本地多优先级队列消费、全局队列搬运、work-stealing | `Processer.cc:19-33`、`156-296` |
| DnsResolver worker | 懒启动单线程 | 承接 getaddrinfo 等无法事件化的阻塞 libc 调用，协程经 `CoWaiter` 挂起 | `DnsResolver.hpp:16-29` |

调度线程主循环节拍（`Scheduler.cc:245-251`）：`_OnUpdate()` 内部 `do{ PollOnce(); _FixTimingScan(); StackPool::OnUpdate(); }while(actived)`——只要 EventLoop 持续报活跃就连转，不插入 sleep；`_OnUpdate` 返回后以 `sleep_until(prev + m_cfg_scan_interval_ms)` 对齐 `m_cfg_scan_interval_ms`（默认 1ms，`GlobalConfig.hpp:39`）周期。

另有三种启动形态：后台调度线程（`SCHE_THREAD`）、当前线程跑循环（`SCHE_LOOP`）、手动 `LoopOnce` 驱动（`SCHE_NO_LOOP`，供嵌入既有事件循环；`Scheduler.cc:254-293`）。

### 1.2 队列拓扑与任务流向

- **新协程**走 `_LoadBlance2Proc` 按 `m_load_idx` 轮询直投某个 worker 的本地队列（`Scheduler.cc:59-79`、`424-438`），**不经全局队列**；放置决策不看负载（`GetLoadValue` 存在但未被放置路径使用，`Processer.cc:56-59`）。
- **被事件/超时唤醒的协程**统一进全局优先级队列 `m_global_coroutine_queue[4]`（`Scheduler.cc:102-133`），由 worker 本地队列空时按 `m_cfg_processer_get_co_from_g_count`（16，`GlobalConfig.hpp:44`）批量搬回，或预算未耗尽时逐优先级单条补充（`Processer.cc:181-191`、`332-348`）。
- 队列为 `moodycamel::BlockingConcurrentQueue` 按 `Coroutine*` 存取（`Define.hpp:332-333`），实际以 `try_dequeue` 非阻塞消费，休眠靠 `m_run_cond` 条件变量（`Processer.cc:281-288`）。

### 1.3 公平与优先级机制

- 四级优先级 `LOW/NORMAL/HIGH/CRITICAL`（`Define.hpp:303-310`）；每公平轮次内按**运行时间预算加权轮转**，预算 `m_cfg_processer_priority_runtime_budget_us{50,150,200,600}` µs（`GlobalConfig.hpp:49-51`，`Processer.cc:128-153`、`178-249`）。这是**队列级公平，不是对运行中协程的抢占**——预算只在协程让出后结算，无法打断正在运行的用户代码。
- MLFQ：事件唤醒时若上次运行 `m_last_run_us > 1ms` 则降入 `LOW`（上限 3 次），运行收敛则逐级回升；`TIMEOUT` 事件覆盖为 `CRITICAL`（`Coroutine.cc:440-479`）。注意 MLFQ 只作用于事件唤醒路径；`YieldAndPushGCoQueue` 主动让出的协程一律以 `NORMAL` 重新入队（`Processer.cc:241-244`）。
- Work-stealing **存在但门控严格**：`Steal` 仅在被偷方当前协程已连续运行 ≥ `m_cfg_processer_worksteal_timeout_ms`（10ms，`GlobalConfig.hpp:46`）时才放行，一次偷 `max(8, size/2)`（`Processer.cc:400-450`、`Scheduler.cc:440-468`）。即「只在被偷方明显繁忙/疑似卡住时才再分配」，健康但忙碌的 worker 不会被偷。

### 1.4 事件等待生命周期

`CoPollEvent` 把「一次等待」建模为**单个原子状态字**上的 CAS 状态机：`INITED→ARMING→ARMED→PARKED→TRIGGERING→FINAL`，提前触发经 `PENDING`，取消进 `CANCELLED`；触发 flags 与阶段打包在同一 uint64（`Define.hpp:212-247`，`CoPollEvent.cc:90-131`、`332-375`）。配套两个关键手法：

- `YieldWithCallback`：先切回 worker 上下文、确认协程已挂起后再注册事件，消除「未挂起先唤醒」竞态（`Coroutine.cc:89-103`，`Coroutine.hpp:85-97` 注释明确动机）。
- `CommitPark`：`ARMED→PARKED` 是发布等待的尾操作；若触发先胜（`PENDING`），由当前 worker 就地消费 flags（`CoPollEvent.cc:332-375`）。重复 `Trigger` 只有第一次有效（`CoPollEvent.cc:90-131`）。

事件销毁走延迟队列：底层 `Event` 只能在「第一个调用 `PollOnce` 的驱动线程」上析构，`DeferDestroyEvent`/`FlushDeferredEvents` 保证同一 FD 不会在 ASIO 重复注册（`CoPoller.cc:54-113`，`CoPoller.hpp:24-28` 注释）。另设 fd→waiter 登记表 `s_waiters`：`close(fd)` 成功时 `WakeupFdWaiters` 唤醒全部等待者，补偿 Linux close 静默移除 epoll 关注项导致的永久挂起（`CoPollEvent.cc:139-198`、`Hook.cc:274-283`，对应 #262）。

## 2. 设计哲学总结

从代码与契约可提炼六条互相咬合的设计哲学：

1. **协作式调度，放弃抢占。** 契约 §2 明文「不承诺 Go runtime 的抢占或自动恢复能力」；M1 计划风险节承认「有栈协程不能可靠拦截用户代码中的死循环；只能提供 worker 无进展诊断……不能承诺自动恢复」（`agent-docs/2026-09-07-m1-core-contract-plan.md` Risks）。协程只有经 `Yield`/`YieldWithCallback`/`YieldAndPushGCoQueue` 主动让出（`Coroutine.cc:77-115`）。
2. **检测与执行分离。** 事件轮询、停顿扫描、栈池调整全部收在独立调度线程，不占 worker CPU（`Scheduler.cc:222-224`）；worker 只做取任务-执行-让出。`Scheduler.hpp:26` 注释「Scheduler 本身压力较小，希望如果后续有调整放在 Scheduler 中」表明这是刻意的职责收束。
3. **best-effort 软件定时器，精度低承诺。** 定时器本质是 EventLoop 被调度线程以 `m_cfg_scan_interval_ms`（1ms）节拍轮询出来的结果（`Scheduler.cc:222`、`245-251`）；对外契约只承诺 ms 粒度、亚毫秒向上取整（`agent-docs/2026-09-07-m1-04b-hook-timeout-flags-gap.md` 已知限制节），POSIX 语义保留但不承诺与内核逐微秒一致。
4. **可替换 poller backend，但拒绝过度抽象。** `Scheduler` 只经 `CoPoller::PollOnce` 门面驱动（`Scheduler.cc:220-222` 注释「换 backend 不改这里」）；契约 §4 同时锁定「Poller 可被替换」「epoll 不属于用户契约」和「不提前建设多后端插件体系」。当前 `CoPoller` 内直接构造 `bbt::pollevent::EventLoop`（`CoPoller.cc:33-40`，附 `NO_CACHE_TIME|PRECISE_TIMER` flag），接口 `IPoller`/`IPollEvent` 是「未接入遗留接口」（`CoPoller.hpp:24`、`CoPollEvent.hpp:21` 注释）。
5. **诊断而非强制中断。** worker 停顿只做观测上报——seqlock 快照 → `WorkerStallInfo`（co id、desc、已运行时长、本地积压）→ 回调或 stderr，同一停顿只报一次（`Scheduler.cc:135-201`、`Define.hpp:273-293`）；协作式取消 `RequestCancel` 只置位 + 唤醒等待，运行中代码在 `YieldUntil*` 入口等检查点自查 `IsCancelRequested` 退出（`Coroutine.cc:139-149`、`168-171` 等）；契约 §8 定调「允许故障，但不能静默失败」，且明确「不建设完整监控平台」。
6. **透明兼容 + 显式降级。** Hook 用 `dlsym(RTLD_NEXT)` + `extern "C"` 符号拦截（`Hook.hpp:63-89`、`127-155`），仅在 `EnableUseCo()` 的 worker 线程生效，其余线程直通原生（`Hook.cc:1196` 起各 `extern "C"` 入口）；契约 §5 要求保 `errno`/flags/超时语义、无法安全转换时必须明确降级。`ErrnoGuard`/`IoTimeout`/`CoIoNonblockGuard`/`FileOffsetGuard` 等一批 RAII 守卫是这一哲学的实现载体（`Hook.cc:81-220`）。

## 3. 优点评估

1. **关注点分离干净。** 事件检测（Scheduler 线程）与任务执行（worker）解耦：定时器到点、fd 就绪的检测不依赖任何 worker 有空——worker 全忙时事件仍能按时被检出并入全局队列（`Scheduler.cc:222`、`102-133`）。对比把 epoll 内嵌在每个 worker 的方案，检测路径的延迟方差与 worker 负载无关。
2. **诊断零开销可选。** `_FixTimingScan` 在阈值=0 时整段跳过（`Scheduler.cc:139-141`）；开启后读的是 worker 侧的 seqlock 锁存快照，执行路径仅在 Resume 前后各写一次原子标志（`Processer.cc:206-228`），不引入互斥锁。`Test_worker_stall.cc` 验证了 spin 检出、单次上报、parked 不误报三条契约。
3. **backend 替换面小。** Scheduler 与 epoll/ASIO 之间只有 `PollOnce/CreateEvent/NotifyCustomEvent/DeferDestroyEvent/FlushDeferredEvents` 五个门面函数（`CoPoller.hpp:39-60`），换 backend 不动调度器；契约把「可替换」与「epoll 非用户契约」同时写入，避免用户代码对具体机制形成依赖。
4. **低上下文切换开销与可预测性。** 有栈协程 + boost.context `fcontext` 切换（`Context.hpp:10`），无信号/抢占点带来的不可预计中断；队列级预算轮转与 MLFQ 都在让出点结算，单线程视角下用户代码运行区间确定——对延迟敏感且任务粒度可控的场景，协作式的「不意外的停顿」本身是优点。
5. **精度契约明确、不过度承诺。** 1ms 粒度、向上取整、`Hook_Connect` 不接 IoTimeout 等都以「已知限制」形式写死在文档与代码注释（`agent-docs/2026-09-07-m1-04b-hook-timeout-flags-gap.md`、`Hook.cc:137` 注释），用户契约与实现一致，不存在「宣传毫秒实际看运气」的落差。
6. **等待竞态的系统性解法。** `YieldWithCallback` + `CommitPark` + 单字 CAS 阶段机，把「注册晚于触发」「触发先于 park」「重复触发」「取消与触发竞争」四类竞态全部收敛进状态字裁决（`CoPollEvent.cc` 全篇），测试 `Test_copollevent_state` 14 例与 `Test_eventloop_contract` 3 例锁住了对外可见行为。
7. **工程细节扎实。** DNS 等不可事件化的阻塞调用有专职线程承接（`DnsResolver.hpp:16-29`）；栈池以 EWMA 均值（0.875/0.125）每 5s 动态调整（`StackPool.cc:60-91`），摊薄 mprotect 栈成本；close(fd) 唤醒表堵住 Linux epoll 静默移除的真实坑（`CoPollEvent.cc:182-198`）。

## 4. 局限与风险

1. **无抢占 ⇒ 长任务拖垮尾部延迟（契约级限制）。** 单个 ≥ 时间片的协程会占死其 worker：本地队列内其他协程最长要等它让出；救济只有两条——(a) 其他 worker 在满足「被偷方当前协程已跑 ≥10ms」门控后偷走其本地队列（`Processer.cc:414-420`）；(b) `_FixTimingScan` 上报。即最坏情形下到点的协程要等 ~10ms+ 才被重新安置，且只「报告」不「治疗」。
2. **准时性依赖 worker 可用性与 OS 调度。** 唤醒链路是 EventLoop→全局队列→worker 搬运→Resume，任何一段都可能延迟；尤其 **`OnActiveCoroutine` 入全局队列不通知任何 worker**（`Scheduler.cc:102-133` 无 notify 调用），worker 休眠路径是 `m_run_cond.wait_for(m_cfg_processer_proc_interval_us)`（默认 3000µs，`Processer.cc:281-288`、`GlobalConfig.hpp:47`）。全部 worker 空闲时，一个到期协程最坏要等 ≈3ms 才被拾取——与 1ms 粒度契约形成~3-4ms 的实际唤醒抖动。调度线程自身也是普通线程，`sleep_until` 受 OS 调度影响无硬保证（`Scheduler.cc:249-250`）。
3. **扫描粒度引入固有延迟。** 事件检出频率= `m_cfg_scan_interval_ms`；`_FixTimingScan` 每拍扫一次，停顿告警延迟 ∈ [threshold, threshold+interval]（`Scheduler.cc:222-224`）。`_OnUpdate` 在事件持续活跃时 `do…while(actived)` 连转不睡（`Scheduler.cc:207-225`）——对响应性有利，但意味着持续事件流下调度线程不退出让 CPU，也无单拍事件数上限。
4. **对用户存在隐性纪律要求。** 协程内必须主动让出/调用 Hook 已覆盖的接口；未覆盖的阻塞调用在协程上下文会直接占死 worker（Hook 仅在 `EnableUseCo` 线程生效，`Hook.cc:1196` 起）。`Hook_Sleep` 在非协程上下文触发 `AssertWithInfo`（`Hook.cc:287`）——`EnableUseCo` 是「本线程可跑协程」而非「当前在协程内」，worker 线程上协程外的调用点是个边缘陷阱。同一 fd 跨线程并发 Hook IO 明确不支持（`agent-docs/api-reference.md` 约束节）；blocking fd 临时置 `O_NONBLOCK` 存在「多协程共享同一 blocking fd 可能竞态」的在案 ponytail（`Hook.cc:90-95`）。
5. **放置策略无视负载 + 全局锁。** 新协程 round-robin 不看积压（`Scheduler.cc:424-438`）；全局队列所有入队/停机排空共用 `m_global_queue_mutex`（`Scheduler.cc:113-130`、`306-309`），唤醒风暴下单锁串行化。队列本身 MPMC 无锁，但 `OnActiveCoroutine` 在锁内做 stopped/stale 判定+enqueue，高并发唤醒时该临界区是全局串行点。
6. **单调度线程是可扩展性天花板。** 全部 fd/timer 事件经一条线程 `PollOnce` 串行检出与回调执行（含 `OnCoPollEvent` 里的 MLFQ 判定、`FlushDeferredEvents`、入队）；事件吞吐上限=单核处理能力。多 backend/多 EventLoop 已被契约显式排除在 M1 外（契约 §4），短期不是缺陷，长期是容量边界。
7. **detached 模型的代价。** 无 join，异常走 `OnException`+`exception_ptr` 保存、无接收者时 `m_unhandled_exception_count` 计数或回调（`Coroutine.cc:128-137`、`GlobalConfig.hpp:70-72`）；`Stop` 对 parked 协程直接 delete 不做栈展开——契约 §6 明示「栈上锁、fd、缓冲等资源在 Stop 路径不被释放」（用户决策 #338）。这是换取轻量 API 的明确取舍，但对「协程内 RAII 保资源」的直觉是危险的：锁必须栈外管理或挂起点前释放。
8. **MLFQ 覆盖不全。** 只有事件唤醒路径做降级判定；纯计算型协程反复 `bbtco_yield` 永远以 NORMAL 回队（`Processer.cc:241-244`），一个「勤于让出但总时长巨大」的协程不会降级。配合第 1 条，预算轮转只能保证跨优先级公平，不能阻止单协程长期占用。

## 5. 与其他方案对比

> 仅在有仓内代码/文档依据处做确定断言；对对方实现细节的描述属于一般性认知，凡未经本仓验证的差异点标注「（外部认知，未在本仓验证）」。

| 维度 | bbtools-coroutine | Go runtime | libco | asio coroutine |
|---|---|---|---|---|
| 抢占 | 无，协作式（契约 §2 明示放弃抢占） | 有（基于信号的异步抢占 + 函数入口栈检查）（外部认知，未在本仓验证） | 无，协作式 | 无（`spawn` 有栈/`co_await` 无栈均协作式）（外部认知，未在本仓验证） |
| 事件检测线程 | 独立调度线程统一 PollOnce（`Scheduler.cc:245-251`） | netpoller 线程 + per-P timer heap 分布式（外部认知，未在本仓验证） | 每个协程线程内嵌 epoll_wait，事件检测占执行线程 | io_context 由用户线程 run()，检测即执行（外部认知，未在本仓验证） |
| 队列/负载均衡 | 全局优先级队列 + 本地队列 + 门控 stealing（被偷方 ≥10ms 才放偷，`Processer.cc:414-423`） | runnext/本地/全局三级 + 常规 stealing（外部认知，未在本仓验证） | 无 stealing，无线程间迁移 | 无内建 stealing，strand/io_context 手工分布（外部认知，未在本仓验证） |
| 优先级 | 4 级 + 运行时间预算轮转 + MLFQ（`GlobalConfig.hpp:51`、`Coroutine.cc:444-456`） | 无用户优先级 | 无 | 无 |
| Hook | `dlsym(RTLD_NEXT)` 符号拦截（`Hook.hpp:63-89`） | 不适用（原生异步 IO） | 同源技术路线（dlsym 拦截），契约 §5 明确「参考 libco」 | 无拦截，API 原生异步 |
| 定时精度承诺 | 1ms 粒度、亚毫秒向上取整（文档明示） | timer 粒度随实现演进、Go 1.23 改无缓冲通道语义（外部认知，未在本仓验证） | 毫秒级，依赖 epoll_wait timeout（外部认知，未在本仓验证） | deadline_timer 精度取决于 backend（本仓 `PRECISE_TIMER` flag 已开启，`CoPoller.cc:34-36`；ASIO 内部实现无法验证） |
| 异常/取消 | C++ 异常边界捕获 + `exception_ptr` 交付；协作式取消 flag+唤醒（`Coroutine.cc:128-149`） | panic/recover + context cancel | 无取消概念（外部认知，未在本仓验证） | 异常经 handler 传播；cancellation slot（外部认知，未在本仓验证） |
| 停机语义 | 取消式，parked 直接销毁不展开（契约 §6，`Scheduler.cc:341`） | 无 runtime 停机概念 | — | io_context::stop 不执行已就绪 handler 由用户负责（外部认知，未在本仓验证） |
| 调度单元语义 | detached，单协程同时只属于一个 worker，唤醒后允许迁移（`Scheduler.hpp:28-31`） | goroutine 可自由迁移 M/P | 协程绑定线程不迁移（外部认知，未在本仓验证） | awaitable 跨线程恢复依赖 executor 语义（外部认知，未在本仓验证） |

**核心取舍差异**：bbtools 选择了「libco 式透明兼容（Hook）+ Go 式多 worker 语义 + 集中式检测线程」的混合路线，用放弃抢占换来 C++ 侧的确定性与低开销，再用独立调度线程把「检测不可抢占」造成的盲区（worker 全忙时无人看事件）单独解决。Go 选择抢占换取尾部延迟保证，代价是运行时不透明性与实现复杂度；libco 简单但没有跨线程均衡；asio 把全部调度策略留给用户。bbtools 的混合使它对「既要 C++ 控制感、又想要协程化既有同步代码」的自用服务定位成立（`agent-docs/2026-09-07-usage-readiness-assessment.md` 结论：受控试点底座），但不是通用抢占式 runtime 的替代品。

## 6. 改进建议

按「成本/收益」排序，均不改契约语义：

1. **全局队列唤醒信号（收益最高、改动最小）。** 在 `OnActiveCoroutine` 入队后挑一个空闲 worker `notify_one`（复用 `AddCoroutineTask` 的 `m_run_cond_notify` 机制，`Processer.cc:83-85`），消除全空闲场景下 ≈`m_cfg_processer_proc_interval_us`（3ms）的拾取延迟。风险：唤醒风暴下的 notify 放大，需退避（如只通知 `PROC_SUSPEND` 状态者，`ProcesserStatus` 已有该枚举 `Define.hpp:175-181`）。
2. **负载感知放置。** `_LoadBlance2Proc` 轮询改为扫 `m_load_blance_vec` 取 `GetExecutableNum()` 最小者（`Processer.cc:61-70` 现成），或维护近似负载直方图；避免新任务均匀落到已被长协程占住的 worker。
3. **停顿处置策略分级。** `m_cfg_worker_stall_warn_ms` 增加第二档「处置」回调（或复用 `m_ext_worker_stall_callback` 返回值）：允许用户注入行动（写日志→打指标→`RequestCancel` 该协程→外部重启），把「诊断但不干预」升级为「诊断且可选择干预」，保持默认零开销不变。
4. **更细扫描节拍可选。** `m_cfg_scan_interval_ms` 已可配（`GlobalConfig.hpp:39`）；可再加「忙时连转、闲时自适应退避」策略替代固定 1ms——当前 `_OnUpdate` 已是忙时连转（`Scheduler.cc:207-225`），缺的是闲时 `sleep_until` 的阶梯退避，降低空载 CPU。
5. **MLFQ 覆盖补齐。** 把降级判定扩展到 `YieldAndPushGCoQueue` 路径（`Processer.cc:241-244` 现在无条件 NORMAL），让频繁让出的长任务也能被压入 LOW，缩小与事件路径的策略缝隙。
6. **worker 休眠参数公开。** `m_cfg_processer_proc_interval_us`、`kGlobalCheckInterval`（4，`Processer.hpp:99`）目前为内部调优；若在文档中标注其对唤醒延迟的影响，用户可按延迟敏感度自调。
7. **单拍事件数上限。** `do…while(actived)` 加 `kMaxEventsPerTick` 防 EventLoop 饥饿循环把调度线程烧穿（`Scheduler.cc:207-225`）；需配合第 4 条退避策略，避免削峰反成延迟源。

## 7. 无法验证项清单

| 论断 | 状态 | 原因 |
|---|---|---|
| `bbt::pollevent::EventLoop` 内部定时器分辨率、`PRECISE_TIMER` 实际语义 | **无法验证** | 实现在 `bbt_core`（外部依赖），本仓仅有 `CoPoller.cc:33-36` 的构造与 flag 注释 |
| backend 是否真用 ASIO | **仅注释依据** | `CoPoller.cc:33` 注释声明「首个 backend：…（ASIO）」，本仓无 ASIO 代码可核 |
| Go/libco/asio 对方实现细节 | **外部认知** | 非本仓代码，§5 表中已逐项标注 |
| `PRECISE_TIMER` 开启后真实唤醒误差分布 | **无法验证** | 需实测 + backend 内部知识；本仓单测只锁 ≤2s deadline 内触发（`Test_eventloop_contract.cc:28-35`） |
| `size_approx` 假阳性频率 | **无法验证** | `Processer.cc:252-257` 注释承认其存在，无测量数据 |

## 8. 结论

bbtools-coroutine 的设计哲学可一句话概括：**以协作式换取 C++ 的确定性，以独立调度线程换取检测的独立性，以显式契约换取可信边界，以诊断而非干预换取实现简单。** 六条哲学在代码里互相咬合且都被测试或文档显式锁定，没有发现「注释宣称一套、实现另一套」的明显漂移。

它的适用边界与哲学是自洽的：任务粒度可控、能遵守让出纪律、追求低延迟方差而非绝对吞吐的自用服务底座——这正是 `usage-readiness-assessment` 给出「受控试点」定位的原因。它不试图成为 Go runtime 的 C++ 复刻，契约 §2 的「不承诺抢占」不是缺陷而是明示的设计立场；真正的尾部延迟风险在 §4 第 1、2 条已按代码量化（~10ms stealing 门控、~3ms 空闲拾取），可被第 6 节的低改动建议覆盖。

## 参考文件

- `bbt/coroutine/detail/Scheduler.{hpp,cc}`、`Processer.{hpp,cc}`、`Coroutine.{hpp,cc}`、`CoPollEvent.{hpp,cc}`、`CoPoller.{hpp,cc}`、`GlobalConfig.{hpp,cc}`、`Define.hpp`、`Hook.{hpp,cc}`、`DnsResolver.hpp`、`StackPool.cc`、`Context.hpp`
- `agent-docs/2026-09-07-core-runtime-contract.md`、`2026-09-07-m1-core-contract-plan.md`、`2026-09-07-m1-04b-hook-timeout-flags-gap.md`、`2026-09-07-usage-readiness-assessment.md`、`asio-cross-platform-plan.md`、`api-reference.md`
- `unit_test/Test_worker_stall.cc`、`Test_eventloop_contract.cc`、`Test_copollevent_state.cc`、`Test_cond.cc`
