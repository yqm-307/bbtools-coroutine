# bbtools-coroutine 核心运行时契约

**状态：** M1 已确定的目标契约（2026-09-07）
**权威级别：** 核心框架设计与实现的唯一契约真源
**适用范围：** 核心运行时层；工具层规则见下文边界；与上层 driver / 网络 I/O 域的分层边界见 §11

> 本文记录已经确定的目标语义，不等同于当前代码已经全部实现。实现、测试、README、Agent 文档与本契约冲突时，先标记差异；未经用户确认不得用当前行为反向修改契约。

## 1. 分层

### 核心框架层

核心框架以后允许大幅替换内部实现，但尽量不改变本契约：

- 有栈 `Coroutine`；
- `Scheduler`；
- `EventLoop`；
- `CoroutineEvent`；
- `Timer`；
- `Hook Adapter`；
- 通用等待、唤醒、超时、取消和关闭机制。

### 工具层

以下属于可迭代工具层，不作为核心状态机的组成部分：

- `Chan`；
- `CoMutex`；
- `CoRWMutex`；
- `CoCond`；
- `CoPool`；
- RAII guard；
- 工具型语法宏和便利封装。

工具层优先保持兼容，但重大变化允许破坏；变化必须同步更新契约、测试和迁移说明。

## 2. 执行模型

- Go 语义优先；C++ 的异常、RAII、所有权和线程边界差异必须显式定义，不承诺 Go runtime 的抢占或自动恢复能力。
- 继续采用有栈协程；本阶段不迁移到 C++20 原生 coroutine。
- 保留当前 detached 协程模型，不强制引入 `Task/Join`。
- 支持多 worker 并行。
- 单个协程同一时刻只能在一个 worker 上运行。
- 协程挂起后允许恢复到其他 worker。
- 继续使用全局单例运行时；`g_scheduler` 是默认且当前核心设计的一部分，本阶段不做多实例。
- 协程取消采用协作式取消：运行中的用户代码不被强杀；等待中的协程可被唤醒。运行中被取消的协程经正常 C++ 栈展开和 RAII 清理。没有「取消式停机」例外：`Scheduler::Stop()` 已删除，运行时是进程寿命对象（见 §6），不存在取消式停机路径。

## 3. 异常与运行时结果

采用 C++ 异常与显式运行时状态的混合模型：

- 用户任务内部允许抛出 C++ 异常，以利用栈展开和 RAII。
- 异常不得逃出 `fcontext` 或 worker 执行边界。
- 运行时边界捕获异常，并以 `std::exception_ptr` 保存和交付。
- 对提供任务结果或 future 的工具 API，结果被取用时默认重新抛出保存的异常；这不要求核心新增 Task/Join。
- detached 任务没有接收者时，异常必须日志 + 计数，禁止静默丢失。
- `Timeout`、`Cancelled`、`Closed`、`Rejected` 等预期运行时结果使用显式状态或返回值，不强制转换为异常。`SchedulerStopped` 是**已废止（superseded）**的历史结果：没有停机入口后不存在「因调度器停止」的结果，见 §6。
- 异常回调自身抛出的异常必须被隔离，不能破坏调度器或 worker。

## 4. EventLoop 与 Poller

当前 `CoPoller` 基于 `bbt::pollevent::EventLoop`；Issue #12 P2 起 `bbt/pollevent` 已内置本仓并随 `bbt_coroutine` 一同构建，不再依赖外部 bbtools-core。M1 固定的是本项目运行时层的职责边界，不直接冻结或修改该事件层的公共 API；涉及 `bbt/pollevent` 变更时，必须同步评估运行时层契约和兼容性。

`EventLoop` 是核心稳定抽象，首版只承诺三类事件：

- FD；
- Timer；
- Wakeup。

职责边界：

- `Scheduler` 负责协程调度，不直接依赖具体 `Poller`。
- `EventLoop` 负责驱动 FD、Timer、Wakeup，并把结果交付给等待者。
- `Poller` 只负责底层就绪事件注册、删除和等待，可被替换。
- `CoroutineEvent` 负责一次等待的生命周期、状态、超时、取消、关闭和协程唤醒。
- 第一阶段只沿现有 Poller/协程事件边界做必要解耦，不提前建设多后端插件体系。
- 首个 backend 可以继续使用现有 epoll 实现，但 epoll 不属于用户契约。
- 本 EventLoop 属于 runtime 内部基础设施，只由调度驱动线程经 `PollOnce` 推进；与上层自有 context / driver 的关系见 §11。

## 5. Hook 契约

Hook 属于核心适配层，目标是参考 libco 的方式，尽可能将其他仓库的同步阻塞调用无损转换为当前协程等待。

- 优先透明兼容主流 POSIX 阻塞调用。
- 尽量保持原生返回值、`errno`、flags、超时、EOF、关闭和线程可见语义。
- 外部创建、进程启动前创建和第三方库创建的 FD 都属于兼容验证范围。
- `MSG_DONTWAIT` 必须保持立即返回语义。
- `SO_RCVTIMEO`、`SO_SNDTIMEO` 等 socket timeout 不能静默变成无限协程等待。
- `EINTR`、`EAGAIN`、`EWOULDBLOCK`、`EBADF`、`EOF` 等错误和边界结果不得被吞掉或误转成功。
- 无法安全转换时必须明确降级、原生直通或返回明确错误；禁止静默改变为协程等待。
- `close` 的代际/closing/waiter 摘除在准备分配完成后一次发布；准备失败不得遗留半发布状态。Hook 边界不让准备阶段 C++ 异常外泄：一次 `bad_alloc` 后只重做尚无副作用的准备，不重试可能已执行的 syscall；再次分配失败返回 `-1/ENOMEM`，其它准备异常返回 `-1/EIO`。
- 已 PARKED 等待的完成赢家在延迟销毁提交扩容失败一次时，仍持有原 Event，在同一临界区有界重做无副作用的提交，再完成唯一唤醒；不回退首胜状态，不在通知线程销毁底层 Event。本保证针对该分配点单次失败且随后内存恢复，不承诺持续 OOM、mutex 损坏、用户回调或其它调度分配失败的通用恢复。
- 是否支持某个调用，以真实兼容矩阵和客户端验证为准，不以 Hook 函数数量作为完成标准。

## 6. 运行时寿命与无停机入口

核心运行时不提供业务停机入口：`Scheduler::Stop()` 已删除，不留兼容壳，也不改名藏到退出钩子。运行时的寿命是**进程寿命（process-lifetime）**：

- 没有「停止接收新任务」状态：`RegistCoroutineTask` 只在队列/Processer 容量不足时报错；
- 不取消、不唤醒、不销毁等待中的协程；
- 不 join worker、不做运行时资源回收；
- 不提供 restart：成功初始化一次后重复 `Start`（含并发 `Start`）抛 `std::logic_error`，不重置配置/队列、不新增 worker。

运行时可观察的初始化状态是 `Scheduler::IsInitialized()`。必要时序是「worker 与 poller/config 就绪 → 发布初始化标志 → 进入运行循环」；标志在初始化完成后恒为 true，不因业务关闭归零。使用 `SchedulerStartOpt::SCHE_START_OPT_SCHE_LOOP`（`Start` 不返回）时，这是外部唯一能观察到初始化完成的时刻。

运行时及其依赖（poller、全局配置、Scheduler 单例、DNS worker 池）按同一进程寿命策略不在静态退出期析构：删除 Stop 后静态析构会直接销毁仍被 worker 线程使用的对象，因此单例由刻意泄漏的持有者维持，且不使用 `std::thread::detach` 代替对象寿命论证。

旧契约中「`Scheduler::Stop()` 对仍挂起协程直接销毁、不做栈展开」是**已废止（superseded）**的历史语义（2026-09-12 #338 的用户决策）：当前实现没有 Stop 路径，因此该例外不再存在。挂起协程「不做栈展开」只是「不存在业务停机」的推论；协程级取消仍按 §2 协作式取消执行，等待中的协程可被唤醒并经正常路径返回。

工具层（例如 `CoPool`）的停止与排空语义独立定义，不依赖运行时停机：`CoPool::Release()` 不再把「运行时正在停机」当作等待逃生条件，普通线程与协程调用路径都必须在池排空期间维持进展。

## 7. 用户入口

- 保留 `bbtco`、`bbtco_sleep`、`bbtco_yield` 等宏，降低迁移成本。
- 稳定契约落在 C++ 类型和函数 API。
- 宏只做薄封装，不承载额外生命周期语义。
- 以后新增或修正宏时，必须能映射到已有稳定 C++ API，不得由宏偷偷引入另一套状态机。

## 8. 故障暴露与诊断

核心运行时必须满足“允许故障，但不能静默失败”：

- 异常、Hook 失败、取消和关闭都必须有可观察结果。
- 最小协程现场包括：协程 ID、业务描述、状态、等待类型、等待对象和等待时长。
- 应能识别 worker 长时间无进展、用户死循环和外部 blocking FD 导致的调度停顿。
- 普通运行路径保持低开销；严格调试和详细 Profile 可作为增强模式。
- 不建设完整监控平台作为 M1 前置条件；先提供可靠日志、计数、快照和必要告警。

## 9. 实现优先级

M1 按以下顺序推进：

1. 协程生命周期与状态机；
2. 取消、等待唤醒、异常和 Stop 语义；
3. 稳定 C++ API 与宏薄封装；
4. EventLoop / Poller / CoroutineEvent 解耦；
5. Hook 语义保持和兼容矩阵；
6. 故障现场与 worker 无进展诊断；
7. Release 构建、工具层边界和真实客户端验收。

## 10. 变更纪律

- 核心实现可以重写，但不得未经确认修改本契约。
- 发现当前实现与契约不一致时，优先建立回归测试并记录差异，再修实现。
- 工具层可以独立迭代，但不得把工具层临时行为提升为核心契约。
- Agent 在设计、实现、审查和验证前必须读取本文；若本文与任务描述冲突，以用户最新明确决策为准，显式记录冲突与用户决策后再更新本文。

## 11. Processer driver 与上层 I/O 域边界

> 2026-10-18 用户接受的 Driver/Reactor 方向及 R1 独立设计复审 `proceed_B1`，替代本节旧的「额外独立网络线程、Processer 只 Resume、不新增 runtime API」目标。这里固定分层契约，不代表 infra 消费已实现或两仓联调通过；B1 仅建设 coroutine 通用支撑。§6 的进程寿命、单次初始化和无 Stop/restart 契约不变。

### 11.1 runtime 组成与线程模型

- runtime = `Scheduler`、多 `Processer`、有栈 `Coroutine`，以及自身等待/唤醒/定时/取消/Hook 使用的内部 `EventLoop`/`CoPoller`、同步工具和对象寿命管理；既有 FD/Timer/Hook 不迁出。
- 默认 `SCHE_START_OPT_SCHE_THREAD` 的 S 线程仍经 `PollOnce` 驱动内部 EventLoop，并做调度扫描与栈池维护；每个 Processer 的 P 线程执行受管协程，并可在非业务协程调度边界推进一个通用 driver。driver 不是新线程、I/O 协程或第二 runtime。
- `SCHE_START_OPT_SCHE_LOOP` 在调用线程运行 S 循环、Start 不返回；`SCHE_START_OPT_SCHE_NO_LOOP` 仍由调用方 LoopOnce 驱动内部 poller。三种模式都不改变 P/driver 的执行边界。
- 协程首次执行可被 stealing 改变，挂起后也可迁移 P。定向首投只是入目标 worker 初始队列的偏好，不承诺首次 Resume、后续恢复、OS/CPU 硬亲和、实时性或故障隔离。

### 11.2 通用 adapter 与动态单槽绑定

- adapter 不包含 Asio、socket 或其它第三方类型；Scheduler 仅对初始化完成后的已登记 worker 提供枚举、绑定、状态查询、同代同步解绑和定向首投。不得用 `GetLocalProcesser()` 惰性创建的未登记 P 充当目标。
- 一个 P 至多一个 driver；允许启动后动态绑定，不抢占已有 Bound、Failed 或 Detaching 的归属。driver 不持 P 的强引用；runtime 按绑定代持有 driver 强引用。
- 成功绑定产生包含 P 身份与非零代次的 token；解绑精确校验同代身份。过期 token 不能解绑新一代，也不能作为旧代成功解绑的收据。并发同代解绑只能确认该代，不得误操作后续重绑。
- 注册前未初始化、非法目标、空 driver、占用、过期 token、自解绑等均须有明确结果。定向首投只接受已登记 P；失败不伪造目标成功，也不改变后续全局恢复和 stealing 路径。

### 11.3 Tick、Wait、Wake 与锁外调用

- `OnTick(BeforeSchedule/AfterSchedule)` 每个调度轮次各至多一次，正常 Bound 连续轮次中 before 在调度前、after 在调度后；公平轮次的 continue 也经过 after。Tick 必须短、非阻塞，不挂起、不直接 Resume。
- Tick/Wait 只在绑定 P 的 worker、非业务协程上下文调用。Wait 仅在本地、global 与 stealing 检查均无 runnable 后进入，timeout 上限沿用当前 Processer idle 间隔。Tick/Wait 不声明 noexcept；返回 Failed 或抛异常由 runtime 边界捕获并记录。
- Wake 为 noexcept、线程安全、有界且不等待 worker；允许外线程、并发及嵌套调用。通知必须持久化早到信号，使「检查后、实际 Wait 前」到达的 Wake 仍能打断 Wait；timeout 只作有界等待退路，不是不丢通知的正确性证明。违反 noexcept、永久阻塞或破坏内存不在可恢复边界内。
- 所有外部 driver 方法及 driver 析构必须在整条调用链的 runtime 锁之外；不能只释放叶函数锁。任务发布在锁内只移交 Coroutine、置通知 latch、保留稳定目标，退出 global/map/通知及同步原语外层锁后再 Wake。发布后不得再读取可能已执行并销毁的 Coroutine。
- 保持 #393 的锁内通知 latch、全局激活广播（不按旧 suspend 状态过滤）和睡前队列复查。无 driver 时保留原有调度/条件变量等待路径，不以 3ms 轮询替代通知。

### 11.4 全 callback lease、Failed 与同步解绑

- 状态为 Unbound → Bound → Detaching → Unbound；Tick/Wait 失败转可观察 Failed，保留 token、错误来源、异常及 driver 归属直到合法解绑。Failed 不再发起普通 Tick/Wait/Wake，不停止 P 或其它 worker。
- 所有 Tick/Wait/Wake 都持该代 callback lease。解绑先在绑定锁内禁止新普通 lease，保留该代强引用并预留终止 Wake 的 lease；锁外执行终止 Wake，再等包括外线程 Wake、嵌套调用及终止 Wake 在内的全部 lease 退出，最后解除同代归属。Failed 同样可解绑，不要求 worker 再运行一轮 ack。
- 绑定 worker、受管业务协程以及当前线程持该代任一 callback lease 时，同步解绑在任何副作用前拒绝，避免等待自身或嵌套外调。最后一个 driver 强引用不得在锁内析构。
- 同代同步解绑成功是该代不再由 runtime 调用的确认，不是业务资源已物理关闭、runtime 已停止或 context 已自动转移 owner 的确认。

### 11.5 infra 消费者责任（目标，尚未实现）

- infra 自有 main/sub context 与网络资源；目标为每个 reactor owner 占一个已登记 P 的 driver 槽，由该 P 的非业务协程 Tick/Wait 推进。worker 槽不足明确失败并回滚已绑槽，不自动扩 worker、不回退旧线程模式、不静默改 loop 数。B1 不引入网络 API，也不反向依赖 infra。
- native I/O、事件注册/撤销、mutable buffer、fd close 与终态仍在稳定 owner 串行域。业务 Handler 只在受管协程执行，同连接事件门保证至多一个活跃；handler 可挂起/迁移，不要求与 pump 同 P。bridge 只做有界命令投递、不可变数据/操作状态交接和既有 CoWaiter/任务通知；不直接 Resume、不跨域裸借 mutable ring、不挂起持 OS mutex。
- 正常关闭由原 owner 推进。一次可恢复 Tick/Wait 失败时，infra 控制线程先封口业务/命令接纳、冻结新资源操作并保留已接纳责任，再以同代 token 同步解绑；确认全部 callback 已退出后，控制线程才通过 infra 内部清理入口独占接管旧 context。该清理不重新绑定 driver、不向复用槽提交旧资源、不新增 runtime teardown/owner-transfer/restart API。
- 物理关闭、取消完成 callback 的有界 drain 及旧 context 单执行者检查属于 infra。清理未完成时保持封口、资源可达和可重试状态，不发布 Stopped、不 DetachOwner、不释放 context；物理与 callback 均收口后，才在 context 活着时 DetachOwner 并释放。不得用 stop 丢 callback 伪造完成，也不等待业务 handler 自身返回。
- B1 fake-driver 只验证失败可观察、全 callback 退出后控制线程可单 owner 清理旧 fake 资源、其它 worker 继续进展；不代表真实 Asio、TCP、Close 或两仓集成已验证。真实消费者若无法兑现这些前提，B2 须暂停并独审最小修正，不降低关闭成功条件。
- 上层仍负责 readiness/drain/EAGAIN、单 callback 及跨 tick 预算、部分写和背压。Asio 公共 async_wait 不等价 native per-fd EPOLLET；外层 poll_one 数量预算也不自动约束 callback 内部耗时。

### 11.6 共享 executor 与参考边界

- `io::GetExecutor()` 仍返回 runtime 内部 CoPoller 全局 EventLoop 的共享 executor，只由现有 S/PollOnce 兑现；不得对该共享 context 额外 run/poll/stop/restart。infra driver 目标使用其自有 context，不改变共享 executor、既有 Hook 或 HTTP/Redis/Mongo/config 等适配。
- gnet（v2.10.0，参考 SHA `58b59a1`）Unix 普通布局为 main/sub goroutine/poller，OnTraffic 在 eventloop 栈调用；goroutine 不等于固定 OS 线程。本项目有意将短 pump 与可挂起的受管业务协程分层，不逐字节移植、不冒充 gnet 原实现。

## 相关文档

- M1 计划：`agent-docs/2026-09-07-m1-core-contract-plan.md`
- 使用期评估：`agent-docs/2026-09-07-usage-readiness-assessment.md`
- 仓库通用 Agent 规范：`AGENTS.md`
- 网络域的 infra 侧文档（B2 须核对并更新，B1 未修改或验证）：bbtools-infra 仓 `docs/architecture/gnet-network-execution.md`、`docs/decisions/0010-gnet-network-foundation.md`
