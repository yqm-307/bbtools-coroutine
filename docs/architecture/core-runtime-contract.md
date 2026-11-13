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
- worker 数可手动调整或显式开启自动策略；真实物理线程协作退役，细则见 §6.1–§6.3，不引入 P/M 两层调度模型。
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
- `close` 对「同号 fd 上的等待」的关闭语义（#370+#402 精确边界）：一次等待从建立到完成只承认一个代际。已 park 的等待在 `BeginFdClose` 时被摘表并以 `POLL_EVENT_CLOSED` 唤醒；事件已就绪但协程尚未恢复时发生 close+reuse，恢复侧代际比对失败按 `EBADF` 收口；`BeginFdClose` 与 `EndFdClose` 之间（底层 close 未返回）发起建立的新等待，在建立点观察到 closing 即被锁存为 CLOSED，不得因 `EndFdClose` 解除 closing 而重获新代准入。三者收敛到同一结果：旧操作 `-1/EBADF`，不重试跨代 syscall、不回写旧 flags/偏移、不影响复用后的同号新对象。本承诺只覆盖 Hook IO 的 `close` 交错，不泛化到任意同 fd 并发 IO；`poll`/`select` 的 close 时序不在本轮承诺内。
- 已 PARKED 等待的完成赢家在延迟销毁提交扩容失败一次时，仍持有原 Event，在同一临界区有界重做无副作用的提交，再完成唯一唤醒；不回退首胜状态，不在通知线程销毁底层 Event。本保证针对该分配点单次失败且随后内存恢复，不承诺持续 OOM、mutex 损坏、用户回调或其它调度分配失败的通用恢复。
- 是否支持某个调用，以真实兼容矩阵和客户端验证为准，不以 Hook 函数数量作为完成标准。

## 6. 运行时寿命与无停机入口

核心运行时不提供业务停机入口：`Scheduler::Stop()` 已删除，不留兼容壳，也不改名藏到退出钩子。运行时的寿命是**进程寿命（process-lifetime）**：

- 没有「停止接收新任务」状态：`RegistCoroutineTask` 只在队列/Processer 容量不足时报错；
- 不因业务关闭或 worker 退役取消、唤醒、销毁等待中的协程；显式取消与事件完成仍按正常路径唤醒；
- 不做全局 runtime teardown；worker 缩容可由 runtime reaper 在锁外 join 退役线程，不等于关闭运行时；
- 不提供 restart：成功初始化一次后重复 `Start`（含并发 `Start`）抛 `std::logic_error`，不重置配置/队列，也不以重复启动新增 worker；扩容只经专用控制入口。

运行时可观察的初始化状态是 `Scheduler::IsInitialized()`。必要时序是「worker 与 poller/config 就绪 → 发布初始化标志 → 进入运行循环」；标志在初始化完成后恒为 true，不因业务关闭归零。使用 `SchedulerStartOpt::SCHE_START_OPT_SCHE_LOOP`（`Start` 不返回）时，这是外部唯一能观察到初始化完成的时刻。

运行时及其依赖（poller、全局配置、Scheduler 单例、DNS worker 池）按同一进程寿命策略不在静态退出期析构：删除 Stop 后静态析构会直接销毁仍被 worker 线程使用的对象，因此单例由刻意泄漏的持有者维持，且不使用 `std::thread::detach` 代替对象寿命论证。

旧契约中「`Scheduler::Stop()` 对仍挂起协程直接销毁、不做栈展开」是**已废止（superseded）**的历史语义（2026-09-12 #338 的用户决策）：当前实现没有 Stop 路径，因此该例外不再存在。worker 退役同样不扫描、销毁或展开挂起协程栈；协程级取消仍按 §2 协作式取消执行，等待中的协程可被唤醒并经正常路径返回。

工具层（例如 `CoPool`）的停止与排空语义独立定义，不依赖运行时停机：`CoPool::Release()` 不再把「运行时正在停机」当作等待逃生条件，普通线程与协程调用路径都必须在池排空期间维持进展。

### 6.1 worker 扩缩容与物理回收

> 2026-10-19 接受的动态扩缩容决策修订旧的「不 join worker」限制；进程寿命、单次初始化和无 Stop/restart 不变。控制接口及线程生命周期遵循下述契约，是否满足仍以对应实现与验证证据为准，不以本文替代验收。

- worker 生命周期区分 Preparing、Active、Retiring、Exited、Joined。`PROC_EXIT`/Exited 只表示调度函数发布退出，不证明物理线程返回、用户 TLS 析构或 join 完成。active 数、target 数与 retiring/exited-unjoined 数必须分别可观察。
- 扩容先准备整批 Processer、线程、ready/失败通道及待发布 registry；全部就绪后一次发布。准备期间不接用户任务或绑定 driver；创建/初始化/快照分配失败只清理未公开批次，旧 active/target/mode/样本计数不变，不无限等 ready。
- 缩容整批提交关闭 admission 并移出 active；旧快照只保对象寿命，每次本地入队仍须重新校验。普通投递失败重试 active/global；定向首投对已退役目标明确拒绝。global pull、steal、睡前复查及 driver Wake 均须覆盖门禁。
- worker 完成本次 Resume 后的完整 READY 入队、EVENT_WAIT park 提交或 FINAL 销毁，才在自身调度栈确认退役。停止获取新工作后，以实际 dequeue 排空所有优先级本地 runnable 并可靠转交全局，失败不丢任务；近似队长和 executing=false 不是排空/退出证明。
- reaper 持有稳定线程记录和 Processer 引用，在全部 runtime/driver/队列锁外 join，返回后才发布 Joined、清理记录。用户 thread_local 析构随原 worker 退出，线程局部地址/资源不承诺与 runtime 等寿命；永久阻塞的用户代码或 TLS 析构不承诺有界回收。
- parked 协程仍由全局 runtime 持有，事件完成后在剩余 active worker 恢复，不扫描或强制展开其业务栈。已退役 P 身份不复用。

### 6.2 控制模式、结果与驱动边界

- 手动/自动请求及相反方向请求经同一串行入口线性化。合法手动请求（包括 same-target）切到 Manual 并重置双向样本；只有显式恢复才切回 Auto，模式切换重置样本。未初始化、越界、driver 约束、创建失败、SelfRetire、Busy 等未接受请求不改 target/mode/样本。
- `ProcesserResizeResult` 区分 Ok、Pending（目标已提交但退役/回收尚未完成）、NotInitialized、InvalidTarget、DriverBound、CreateFailed、SelfRetire 和 Busy（暂时不能接受新目标）。控制成功不是物理 join 收据；不得在持自身 gate/生命周期锁的栈上同步等自身退役。
- Bound、Failed、Detaching driver 均 pin 所属 P 到同代归属真正清除；绑定与退役提交须共用 lifecycle/admission 互斥。绑定先提交则跳过该 P；退役先提交则绑定返回 InvalidTarget 且输出 token 不变。没有 driver 迁移协议。
- 默认 THREAD/LOOP 的调度循环推进自动采样与 reaper；NO_LOOP 不创建隐藏 S 线程，自动采样依赖调用方 `LoopOnce()`。手动控制入口与 `LoopOnce()` 均推进有限 reaper 收割；无后续调用时只能报告 join 未完成，不能声称资源已回收。未完成退役批次须有界，冲突请求以 Busy 等明确结果限制物理线程资源积累，不复活旧 P。
- active 快照须持稳定强引用并以 C++17 可用同步发布；所有外部回调、driver/异常对象最终析构和 join 均在 runtime 锁外。Profiler 的 worker registry 同步纳入准备失败和退役清理，不保活已回收 P。

### 6.3 配置与策略信号

- initial 为 Start 前有效 `m_cfg_static_thread_num`；其构造默认仍为 `hardware_concurrency()`，不可用时 1，不尝试识别是否显式设置。显式 static=0 非法；min/max=0 各自回退 initial。解析后 `1 <= min <= initial <= max <= INT_MAX`，不静默截断。
- auto 默认 false；interval=100ms、up/down samples=3、step=1、busy up/down=80/20%、queue up/down=1/0（全局加 active 本地 runnable 近似总数）。interval 在 `[1, chrono::milliseconds::rep 最大值]`，samples 在 `[1, SIZE_MAX]`，step 在 `[1, INT_MAX]`；busy 在 `[0,100]` 且 down<=up；queue 在 `[0,SIZE_MAX]` 且 `1 <= up`、down<=up。auto 关闭也校验所有字段。
- `GlobalConfig::ResolveProcesserScalingBounds` 无副作用校验上述全部规则，成功才写 initial/min/max 输出。核心实现须在 Start 不可逆启动和占用一次性启动状态前调用；非法配置抛 `std::invalid_argument`，修正后可重试，不能留下线程或半初始化状态。
- 扩容为 `queue >= up OR busy >= up`，缩容为 `queue <= down AND busy <= down`，分别连续命中配置样本数；不命中复位该方向。若相等阈值使同拍双向满足，只选择扩容方向；提交后双向计数复位，计数饱和不溢出，每采样拍至多一批且受 step/min/max/退役候选约束。
- busy 为 active 中正在 Resume 的 worker 占比，轻量信号独立于 `stall_warn_ms` 维护；诊断采集仍可关闭。近似 runnable 仅作控制信号，不证明队列空、所有权移交或安全退役。运行期不得并发修改静态配置字段。

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

## 9. 任务与优先级来源

任务、目标、优先级及实时验收状态统一由 [GitHub Issue](https://github.com/yqm-307/bbtools-coroutine/issues) 记录。本文只规定运行时语义，不维护历史 M1 施工顺序或第二套任务列表；旧的 Stop 实施项已被 §6 的无停机契约替代。

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

- adapter 不包含 Asio、socket 或其它第三方类型；Scheduler 仅对初始化完成后的已登记 worker 提供枚举、绑定、状态查询、同代同步解绑和定向首投。枚举是调用时的 active 快照，可能过期，后续绑定/定向投递须重新校验；不得用 `GetLocalProcesser()` 惰性创建的未登记 P 充当目标。退役与 driver 归属互斥见 §6.2。
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

### 11.5 infra 消费者责任（R2 已确认目标，尚未实现/联调）

- infra 自有 main/sub context、网络资源和 Engine 控制域；每个 Reactor owner 占一个已登记 P 的 driver 槽，由该 P 的非业务协程 Tick/Wait 推进，不额外创建网络 I/O 线程。Engine 自动选择空闲已登记槽，以成功绑定 token 为准；不足或竞争失败明确回滚，不抢占、不扩 worker、不回退旧线程、不静默缩 loop 数。B1 不引入网络 API，也不反向依赖 infra。
- native socket I/O、事件注册/撤销、fd close 与 backend 生命周期维持稳定 owner 串行域。**原始短回调与业务协程分层**：infra 原始 OnAccept/OnConnect/OnRead/OnWrite/OnClose 在 owner 的非业务协程边界执行，必须短、非阻塞、不可挂起，同连接不重入且不递归调用户回调；业务处理及可挂起同步外观由上层使用既有 CoWaiter/Chan/任务注册薄封装，业务协程仍可迁移 P。不强制为每连接/就绪自动创建业务协程，不再要求所有网络通知先进入受管业务事件快照。
- 接收 ring 的存储、字节顺序及未消费数据归 infra；允许通过 infra 自有的最小同步访问面安全共享，**不要求 mutable 接收缓冲只能被 owner 消费**。OnRead 只通知，ring 是数据真源；try 访问竞争明确 Busy、零消费，组合 Peek/Consume 在短保护内，裸 view 不越保护、不挂起持 OS mutex。owner 遇竞争退让，保护释放/容量恢复的持久通知与登记后复查由 infra 兑现。所有用户回调和 Notify 在外层锁之外；关闭后已有接收数据仍可读，ring/状态寿命不依赖已销毁 socket/context。runtime 不托管该缓冲，不建设第二接收队列或等待仲裁。
- bridge 仅负责有界命令、数据所有权/操作状态交接与既有通知，不直接 Resume、不要求协程回原 P。AsyncWrite 的提交接纳、本地发送完成和业务回包分开；已接纳操作的部分进度、唯一终态及超时仍在途责任由 infra 诚实提供。普通 fd errno、EOF、协程等待超时/取消不升级为 driver Failed，不自动关闭/重发；业务重试、分帧、请求匹配与连接使用策略归上层。精确网络 API/背压规则只在 infra 文档定义，不变成 runtime 公共 API。
- Engine::Run 在非受管协程、非 owner 的 native 线程运行控制域；Run 本身不建线程。Engine 的窄 lifecycle 接缝由 infra 拥有其单 Engine native runner 与非阻塞物理回收，不新增常驻 control thread，也不让 framework 持有 runner/thread/future/join。宿主须明确保证 runtime 内部 EventLoop 独立持续推进；NO_LOOP 不能因 Run 占用唯一 LoopOnce 执行流而停止 Timer/等待维护。现有 IsInitialized 不是持续进展证明；runtime 不新增启动模式探活或 Stop/restart。网络启动拒绝及 RequestClose/Run 返回语义由 infra 定义。
- 正常网络关闭仍由原 owner 推进；控制域同代同步解绑成功后才可独占旧 context 进行必要的最后清理。RequestClose 封口不等于资源已释放，Run 返回的收口语义由 infra 兑现，不等待上层业务任务返回或外部句柄归零。同步解绑没有 deadline，不捏造可取消解绑或硬时限。
- 一次可恢复 Tick/Wait 失败时，infra 控制循环通过现有 QueryProcesserDriver 按持有 token 有限间隔复查，不依赖 runtime 新增失败 callback；每代明确报告错误、封口并保留已接纳责任，再同代同步解绑。全部 callback lease 退出后，控制线程才通过 infra 内部入口独占清理旧 context，不重绑、不向复用槽提交旧资源、不新增 runtime teardown/owner-transfer/restart。driver Failed 后不在控制域冒充 owner 补调用户原始回调；infra 用控制域错误、可查询冻结状态及内部完成订阅作为明确替代出口。
- 物理关闭、取消完成 callback 的有界 drain 和旧 context 单执行者检查属于 infra。未收口时保持封口、资源可达与可重试，不发布 Stopped、不 DetachOwner、不释放 context；物理及 callback 均收口后才在 context 活着时拆 backend/DetachOwner 并释放。不得用 stop 丢 callback 伪造完成，也不等待业务 handler 自身返回。持续资源耗尽、永久阻塞或不获调度不承诺有界完成，不据此建设自愈/迁移平台。
- B1 fake-driver 只验证失败可观察、全 callback 退出后控制线程可单 owner 清理旧 fake 资源、其它 worker 继续进展；不代表 R2 真实 Asio/TCP/Close 或两仓集成已验证。真实消费者若无法兑现这些前提，B2 须暂停并独审最小修正，不降低关闭成功条件。
- 上层仍负责 readiness/drain/EAGAIN、单 callback 及跨 tick 预算、部分写和背压。Asio 公共 async_wait 不等价 native per-fd EPOLLET；外层 poll_one 数量预算也不自动约束 callback 内部耗时。

此处按用户确认的 R2 修订消费者目标，不修改 §11.3–§11.4 的 runtime 行为或既有 API；本地文档修改不等于发布、依赖升级或实现放行。infra 侧当前目标见其 `docs/architecture/gnet-network-execution.md` 与 `docs/decisions/0010-gnet-network-foundation.md`；双方发布后消费需记录实际契约、源码和链接 SHA。

### 11.6 共享 executor 与参考边界

- `io::GetExecutor()` 仍返回 runtime 内部 CoPoller 全局 EventLoop 的共享 executor，只由现有 S/PollOnce 兑现；不得对该共享 context 额外 run/poll/stop/restart。infra driver 目标使用其自有 context，不改变共享 executor、既有 Hook 或 HTTP/Redis/Mongo/config 等适配。
- gnet（v2.10.0，参考 SHA `58b59a1`）Unix 普通布局为 main/sub goroutine/poller，OnTraffic 在 eventloop 栈调用；goroutine 不等于固定 OS 线程。本项目有意将短 pump 与可挂起的受管业务协程分层，不逐字节移植、不冒充 gnet 原实现。

## 相关文档

- 任务与验收：[GitHub Issue](https://github.com/yqm-307/bbtools-coroutine/issues)
- 当前调用说明：`docs/guides/user-guide.md`、`docs/guides/api-reference.md`
- 仓库通用 Agent 规范：`AGENTS.md`
- 网络域的 infra 侧文档（B2 须核对并更新，B1 未修改或验证）：bbtools-infra 仓 `docs/architecture/gnet-network-execution.md`、`docs/decisions/0010-gnet-network-foundation.md`

## 12. Co-only 入口、对象观测与三仓桥接边界

本节记录当前的 Co-only 入口、对象观测与跨仓责任边界。运行时保持 §6 所述的 process-lifetime、单次初始化、无 `Scheduler::Stop`/restart；主入口由 runtime 的 `RuntimeMain` 拥有（§12.2）。`IsInCoroutineContext()`、`RequireCoroutineContext()`、对象管理与统计入口已实现，并注册对应 CTest。framework/infra 消费这些入口时，不把网络 owner、业务 registry 或物理 fd 责任下沉到 runtime。

当前实现包括 `IsInCoroutineContext()`/`RequireCoroutineContext()`、`CoRuntimeOnly`、`CoObject`/`ManagerBase`/`MemberBase`/`MakeCoObject`/`GetCoObjectStats`，对应测试目标为 `coroutine.runtime_context_no_create`、`coroutine.runtime_context_runtime`、`coroutine.runtime_objects`。主入口目标语义见 §12.2：runtime 生成 native `main` 并负责启动、hook、主任务完成与退出码；不提供 `RunMainTask`/`MainNativeControl`/第二套启动桥。

### 12.1 上下文真源与 `CoRuntimeOnly`

当前源码的上下文真源仍是 `GetLocalCoroutineId()`：它是无创建查询，非受管上下文返回 `0`，不会惰性创建 `Processer`、启动 runtime 或改变状态。公开的最小 API 为：

```cpp
bool IsInCoroutineContext();
void RequireCoroutineContext(); // 非 Co 域抛 std::logic_error

class CoRuntimeOnly {
public:
    static bool IsInCoroutineContext();
    static void RequireCoroutineContext();
};
```

`CoRuntimeOnly` 是无状态、无虚函数的能力标记与检查转发层；它不删除复制/移动，不自动检查构造/析构，不保存 TLS、线程 id、对象 owner 或计数。静态成员只转调 namespace 函数，真源仍为 `GetLocalCoroutineId()`。重要入口在副作用、入队、加锁或提交等待责任前检查；普通 getter、值对象、native callback 的短路径不机械注入检查。Release 构建同样执行 gate；使用 `result` 的重要接口直接调用 bool 查询并返回既有 `InvalidContext`/`operation_not_permitted`，不走 Require→throw→catch，也不让异常穿过非抛出公共接口。

### 12.2 `bbtco_main` 主入口与 `RuntimeMain`

主入口属于 `bbt/coroutine/runtime`：应用定义 `int bbtco_main()`，可选定义 `void setup_runtime()`、提交前 `int bbtco_main_thread_hook()` 与提交后 `int bbtco_main_host_hook(bbt::coroutine::HostHookContext&)`；生成的 native `main` 只调用 runtime 入口。`setup_runtime()` 在 Start 前执行，是设置既有 GlobalConfig 全部启动字段的唯一生命周期阶段；随后 RuntimeOptions 只覆盖 worker 数、默认栈和栈预算三类便捷输入，配置快照冻结后再执行 Start。需要非默认启动参数时，不提供第二个配置 hook 或依赖全局静态初始化。Strict/Auto 在 Start 前校验并应用到既有 StackPool，非法预算以 `StartFailed` 拒绝。

```cpp
namespace bbt::coroutine {

struct StackRequest { std::size_t usable_bytes; bool protect; };
enum class StackPolicy { Strict, Auto };
struct StackClassLimit { StackRequest request; std::size_t max_objects; };
struct StackConfig {
    StackPolicy policy;
    std::vector<StackClassLimit> strict_classes;
    std::size_t auto_max_objects;
    std::size_t auto_max_usable_bytes;
};
struct RuntimeOptions {
    std::optional<std::size_t> initial_workers; // 无值沿用既有默认；有值必须 >= 1
    std::optional<StackRequest> default_stack;  // 主任务及未显式指定栈的内部任务
    std::optional<StackConfig> stacks;          // 无值使用 runtime 的有限默认预算
};

// 提交后 host hook 的运行时控制面：runtime 持有，hook 只登记与观察。
class HostHookContext {
public:
    // 主任务（含未捕获异常）完成时恰好调用一次；hook 运行期任意时刻可登记，
    // 已完成时立即在登记线程回调。回调在 runtime 内部锁之外执行，不得抛异常、不得阻塞。
    // 登记为单槽。寿命只到 host hook 返回或抛异常：此后 runtime 丢弃未取出的登记、
    // 不再发起新回调；与完成路径竞争而已被取出的回调无法撤销，仍可能在 hook 返回后执行，
    // 所以闭包必须以值或自有共享状态捕获，runtime 不保护任意 hook 栈引用。
    void OnMainTaskDone(std::function<void()> callback) noexcept;
    bool MainTaskDone() const noexcept;
};

// 提交后 host hook：在原生 main 线程调用一次；允许宿主 native 阻塞入口，
// 但不得调用 Co 原语、不得提交主任务。
using HostHook = int (*)(HostHookContext&);

class RuntimeMain {
public:
    enum class Terminal {
        MainReturned,   // 主任务正常返回
        HookRejected,   // 提交前 hook 返回非零，主任务未提交
        HookThrew,      // 提交前 hook 抛出异常，主任务未提交
        SetupThrew,     // setup_runtime 抛出异常，Start 未调用
        StartFailed,    // runtime 启动/初始化失败
        SubmitFailed,   // 主任务提交失败
        MainThrew,      // 主任务未捕获异常
        HostHookFailed, // 提交后 host hook 返回非零
        HostHookThrew,  // 提交后 host hook 抛出异常
    };
    struct Completion { Terminal terminal; int exit_code; };

    // 不抛异常；所有失败转换为非零 exit_code。
    static Completion Run(int (*main_task)(), int (*native_hook)() = nullptr,
                          const RuntimeOptions& options = {}) noexcept;
    static Completion RunWithSetup(void (*setup_runtime)(), int (*main_task)(),
                                   int (*native_hook)() = nullptr,
                                   const RuntimeOptions& options = {}) noexcept;
    // 提交后 host hook：主任务提交成功后才在原生 main 线程执行一次。
    static Completion RunWithSetupAndHostHook(void (*setup_runtime)(),
                                              int (*main_task)(), HostHook host_hook,
                                              const RuntimeOptions& options = {}) noexcept;
    // 包住配置表达式的求值/值构造；失败也兑现同一个 StartFailed 完成记录。
    template<class OptionsFactory>
    static Completion RunWithOptions(int (*main_task)(), int (*native_hook)(),
                                     OptionsFactory make_options) noexcept;
    template<class OptionsFactory>
    static Completion RunWithSetupHostHookAndOptions(void (*setup_runtime)(),
                                                     int (*main_task)(), HostHook host_hook,
                                                     OptionsFactory make_options) noexcept;
};

} // namespace bbt::coroutine

#define BBTCO_MAIN() \
    int main() { return ::bbt::coroutine::RuntimeMain::Run(&bbtco_main).exit_code; }
#define BBTCO_MAIN_WITH_HOOK() \
    int main() { return ::bbt::coroutine::RuntimeMain::Run( \
        &bbtco_main, &bbtco_main_thread_hook).exit_code; }
#define BBTCO_MAIN_WITH_OPTIONS(options_expr) \
    int main() { return ::bbt::coroutine::RuntimeMain::RunWithOptions( \
        &bbtco_main, nullptr, [] { return (options_expr); }).exit_code; }
#define BBTCO_MAIN_WITH_HOOK_AND_OPTIONS(options_expr) \
    int main() { return ::bbt::coroutine::RuntimeMain::RunWithOptions( \
        &bbtco_main, &bbtco_main_thread_hook, [] { return (options_expr); }).exit_code; }
#define BBTCO_MAIN_WITH_SETUP() \
    int main() { return ::bbt::coroutine::RuntimeMain::RunWithSetup( \
        &setup_runtime, &bbtco_main).exit_code; }
#define BBTCO_MAIN_WITH_SETUP_AND_HOOK() \
    int main() { return ::bbt::coroutine::RuntimeMain::RunWithSetup( \
        &setup_runtime, &bbtco_main, &bbtco_main_thread_hook).exit_code; }
#define BBTCO_MAIN_WITH_SETUP_AND_OPTIONS(options_expr) \
    int main() { return ::bbt::coroutine::RuntimeMain::RunWithSetupAndOptions( \
        &setup_runtime, &bbtco_main, nullptr, [] { return (options_expr); }).exit_code; }
#define BBTCO_MAIN_WITH_SETUP_HOOK_AND_OPTIONS(options_expr) \
    int main() { return ::bbt::coroutine::RuntimeMain::RunWithSetupAndOptions( \
        &setup_runtime, &bbtco_main, &bbtco_main_thread_hook, \
        [] { return (options_expr); }).exit_code; }
// 提交后 host hook：定义 int bbtco_main_host_hook(bbt::coroutine::HostHookContext&)。
#define BBTCO_MAIN_WITH_SETUP_AND_HOST_HOOK() \
    int main() { return ::bbt::coroutine::RuntimeMain::RunWithSetupAndHostHook( \
        &setup_runtime, &bbtco_main, &bbtco_main_host_hook).exit_code; }
#define BBTCO_MAIN_WITH_SETUP_HOST_HOOK_AND_OPTIONS(options_expr) \
    int main() { return ::bbt::coroutine::RuntimeMain::RunWithSetupHostHookAndOptions( \
        &setup_runtime, &bbtco_main, &bbtco_main_host_hook, \
        [] { return (options_expr); }).exit_code; }
```

`setup_runtime()` 先在 native main 同步设置既有 GlobalConfig；随后 RuntimeOptions 工厂在同一 runtime 异常边界内最多求值一次，显式字段覆盖三类便捷输入。Start 前将全部 scheduler/stack pool 配置固化为 `RuntimeConfigSnapshot`，运行期组件只读取该快照；兼容保留的公开字段即使被后续代码改写，也不改变已启动 runtime 的配置。配置门冻结后固定执行 Start（单次）→ 提交前 native hook → 提交主任务 → 提交后 host hook → 等主任务与 host hook 都结束 → 返回。配置阶段构造、校验或应用失败均为非零失败，不调用 Start/hook、不提交主任务；先完整校验再应用，不留下半份可重试配置。没有 setup_runtime 的旧 `Run`/`BBTCO_MAIN*` 仍兼容，直接 Start 时由 Scheduler 完成冻结。

`stacks` 有值时：Strict 要求非空、规范化键唯一、各 `max_objects > 0`，Auto 的两个字段必须为 0；Auto 要求 `strict_classes` 为空且两个上限均 > 0。校验尺寸/乘加溢出、默认栈是否为 Strict 已声明类，以及启动所需 worker/monitor 的精确规格与容量是否可接纳；不以扩大预算或跨桶分配修复非法输入。无值时的有限默认预算由实现记录，仍须通过同一校验。`initial_workers` 只覆盖既有 initial worker 数，既有 min/max/自动扩缩容校验不被绕过。

非默认消费示例（仅构造值，无启动副作用）：

```cpp
bbt::coroutine::RuntimeOptions MainOptions() {
    using namespace bbt::coroutine;
    RuntimeOptions o;
    o.initial_workers = 1;
    o.default_stack = StackRequest{128 * 1024, true};
    return o;
}
// 定义 int bbtco_main() 后，替代无参数宏：
BBTCO_MAIN_WITH_OPTIONS(MainOptions())
// Strict 可增加：
// o.stacks = StackConfig{StackPolicy::Strict, {{{128 * 1024, true}, 1024}}, 0, 0};
// Auto 输入为 StackConfig{StackPolicy::Auto, {}, 1024, 128 * 1024 * 1024}。
```

约束如下：

- hook 在 runtime Start 之后、native 主线程、任何主协程提交之前运行；不得挂起、不得调用 `bbtco_*`/Co 等待原语，也不能等待 Engine ready/closed/physical-released、执行 Engine::Run/native join 或阻塞等待 Co worker。runtime 配置已在 Start 前冻结；Engine/资源的装配与启动参数在主 Co 任务内、首次 Engine Run 前冻结，不混淆两次 Start。hook 非零则不提交主任务，进程退出码取该返回值；hook 抛异常即 `HookThrew`，按失败退出，不逃出 native main。
- 提交后 host hook 在提交成功之后、同一 native 主线程、主任务并发运行期间执行一次。它只做宿主 native 工作的阻塞入口（framework 用它调用一次 `Engine::Run`），不能调用 Co 原语、不能提交主任务、不能假设主任务已发布任何状态；需要主任务的装配结果时用 `HostHookContext::OnMainTaskDone`/`MainTaskDone` 协调，并自行给有限等待。
- host hook 完成与主任务完成都必须到达后 runtime 才返回。主任务（含未捕获异常）完成时 runtime 恰好一次调用已登记的 `OnMainTaskDone`；登记可在 hook 内任意时刻发生，若主任务已完成则立即在登记线程回调，回调一律在 runtime 内部锁之外执行（不持锁调用 hook 代码）。回调寿命只到 host hook 返回或抛异常为止：hook 先结束时 runtime 在同一线程丢弃未取出的登记，此后不再发起新回调，主任务之后完成既不丢失完成记录（仍兑现唯一 `MainCompletion`）也不会再调用该登记；但清除登记无法撤销已与完成路径竞争而取出的回调，它可能在 hook 返回后仍在执行，因此闭包必须以值或自有共享状态捕获，runtime 不保护任意 hook 栈引用。已发起的回调返回后 runtime 才发布主任务完成记录，`RuntimeMain` 返回时不存在本登记的在途回调；hook 抛异常同样按 `HostHookThrew` 收口并丢弃未取出的登记。该寿命规则只扩展既有单槽登记，不新增通用回调注册表，也不改提交前初始化 hook 与既有 `Run`/`BBTCO_MAIN*` 的兼容面。hook 先返回时 runtime 继续等主任务；主任务先完成时 runtime 继续等 hook 返回。完成记录仍只有一个，优先级：host hook 返回非零 → `HostHookFailed`（取该值）；host hook 抛出异常 → `HostHookThrew`（1）；主任务未捕获异常 → `MainThrew`（1）；否则 `MainReturned`（取主任务返回值）。提交前 hook 非零/抛异常仍为 `HookRejected`/`HookThrew`（主任务不提交）。
- 两向退出是宿主责任，不是 runtime 的取消能力：主任务结束如何让 host hook 的阻塞调用返回（例如对共享 Engine 发 `RequestClose`），以及 host hook 失败如何让主任务结束（例如让主任务的有限等待观察到失败），都由 hook 与主任务之间的应用层约定保证；runtime 只保证通知恰好一次、返回前两边都结束，不提供第二个取消通道，也不用 `result.get()` 前后换位掩盖死等。
- 主任务是一个普通受管协程，使用启动配置的默认栈请求（默认尺寸与保护属性），可正常 `bbt_co_await`。主任务正常返回即 `MainReturned`，进程退出码取其 `int` 返回值；未捕获异常在 runtime 边界转换为非零退出码。
- 启动失败、hook 拒绝、提交失败、主任务返回/异常、host hook 返回/异常各自恰好兑现一次 `MainCompletion`（内部完成记录，仅一个 native 等待者）；重复 `RuntimeMain::Run`、在受管协程内调用或在 hook 中重入返回明确失败，不启动第二次 runtime、不进入永久等待、不重复通知。
- 只等主任务与 host hook，不排空 detached 任务、不新增 `Scheduler::Stop`/restart、不隐式销毁进程寿命对象。业务收口必须在 `bbtco_main` 内显式完成；`main` 返回后未收口责任不转为已关闭。hook 未登记/未调用（旧入口）时行为与旧契约相同：只等主任务。
- runtime 不消费或推进 Engine，不提供 native 网络回收槽。Engine 的创建、共享与关闭语义归应用/framework（Engine 主线程宿主契约 infra `docs/architecture/native-lifecycle-seam-contract.md`）；host hook 是应用自己的函数，runtime 只负责在主任务提交之后、原生 main 线程调用它一次，不 include infra、不认识 Engine、不执行 join，也不替 hook 决定等待期限。
- 失败退出码：`StartFailed`、`SubmitFailed`、`HookThrew`、`HostHookThrew`、`MainThrew` 为 `1`；`HookRejected`/`HostHookFailed` 取 hook 返回值；正常主任务取主任务返回值。`0` 仅表示主任务正常返回且其返回值为 0；OS 对 0–255 之外取值的处理不在 runtime 承诺内。
- `coroutine.runtime_main_*` 通过独立子进程断言真实退出码、配置/Start/hook/submit 顺序、一次执行及主任务完成；覆盖四种生成式宏、非标准异常、启动线程失败、提交分配失败、重入和不排空 detached 任务。新增 host hook 用例覆盖：hook 等主任务发布后才运行、主任务完成触发 host 关闭、完成先于登记（立即在登记线程回调）、登记与完成竞争（恰好一次）、hook 先返回/抛异常、在途回调与 hook 栈退出、host 失败唤醒主任务与退出码优先级、host 与主任务两边都结束才返回。`stack_policy` 覆盖 Strict/Auto 成功启动；具体预算与异步所有权由 `coroutine.stack_ownership_*` 验证。

### 12.3 `ICoObject`、`CoObject` 与统计契约

保留现有 `bbt::coroutine::ICoObject` 及其精确签名：

```cpp
virtual ~ICoObject() = default;
virtual CoObjectInfo GetObjectInfo() const = 0;
```

现有 `CoObjectInfo { CoObjectId id; std::string kind; std::string name; }` 仍是进程内身份快照；它不携带代际、owner、执行域或 fd 状态。coroutine 的 object 层有无反射、无 Boost 依赖的 `ManagerBase<TKey,TMember>`/`MemberBase<TKey,TMember>` 通用机制；它只提供创建提交、成员初始化、销毁通知和最小记账，不建立第二套对象表。`CoObject` 复用现有身份分配，`MakeCoObject` 只是薄转发到同一 Manager creator，不重新分配 ID 或重复登记。

```cpp
class CoObject : public ICoObject { /* existing identity + observation; no owner */ };
struct CoObjectStats {
    std::uint64_t created_total;
    std::uint64_t destroyed_total;
    std::uint64_t alive;
    std::uint64_t peak_alive;
};
CoObjectStats GetCoObjectStats(std::string_view kind) noexcept;

template<class T, class Creator>
auto MakeCoObject(std::string kind, std::string name, Creator&& creator)
    -> decltype(std::forward<Creator>(creator)());
// creator 是立即调用的零参闭包，返回 T 已有的合法 owner/deleter。
```

对象创建口径：`ManagerBase<TKey,TMember>::Create(key, kind, name, creator)` 实施唯一构造/登记/提交，`MakeCoObject<T>` 只是绑定同一 CoObject manager 的薄转发；不保留第二个默认 `unique_ptr<T>` 工厂。creator 在调用者已有权限域形成，只在上下文检查之后调用一次；返回非空 `unique_ptr<T,D>`、`shared_ptr<T>` 或可解引用的合法既有 owner，失败抛既有构造/登记错误。工厂只借用对象做 Member 初始化，不接管 deleter、不缓存 creator 或 owner。framework 可在已有 `ConstructKey` 权限域捕获 key，闭包中以 `Registry::NewManaged<T>(key, ...)` 和 `Registry::DeleteManaged<T>` 建立类型化 owner，观测提交后再转入原有擦除 owner；Registry 的访问权限不转移给工厂或 `default_delete`。

creator 的唯一提交顺序是：先为 Co-only T 做上下文检查；调用合法 creator 完整构造 T；复用对象已有 `CoObjectInfo`（没有时才分配一次）；把 `MemberBase` 初始化为 `weak_ptr<Manager>`、key 和独立最小记账状态；完成观测登记后再提交一次创建计数并把原 owner 交付。受限 protected/private 析构不开放，`std::default_delete<T>` 不是统一前置条件；已有 `unique_ptr<T,D>`、Registry 的类型化擦除 owner 或其他合法 owner 形态都可保留。

创建或观测登记在提交前失败时，原 owner 立即释放，Member 析构不得误发 `OnMemberDestroy`/destroy 记账。外部 framework/infra 安装、依赖绑定和业务发布是另一笔事务；发布前失败时释放已经成功创建的对象，实际析构形成一对 created/destroyed，`alive` 回到安装前基线，不撤销历史 created。已发布对象的 `OnInstall` 失败若按 framework 原语义保留为 `InstallFailed`，则继续计 alive，直到显式卸载及实际析构才计 destroyed；不能为了计数回基线暗改失败状态或提早释放。

Manager 的共享对象索引只保存 `weak_ptr`；查询用一次 `if (auto owner = weak.lock())` 取得短暂局部强引用，立即完成快照后释放，不先 `expired()` 再 `lock()`，不缓存、不跨等待持有。独占对象只保存 ID/kind/name 与计数元数据，不为观测改成 `shared_ptr`。Manager 销毁先于成员时，Member 只依赖独立最小记账状态安全完成一次销毁计数；它不反向保活成员，也不要求调用已经失效的 Manager。失效索引沿既有查询/注销清理，不另起线程。成员析构路径必须 `noexcept`、不挂起、不调用业务虚回调、不访问派生对象；manager/Registry、在途任务和 native 物理回收 owner 的必要强引用仍由各自责任保留。

`Close`/`Stop` 不减 `alive`，句柄移动不重复登记；统计只保存计数与非 owning snapshot，不强持有对象，不依赖 Co 上下文，不抛异常。未知 kind 返回空快照，`noexcept` 查询不得懒分配、调用用户 hook 或依赖静态析构次序；已有类型桶与独立记账状态活到已登记成员完成析构。任一快照满足 `created_total - destroyed_total == alive`。

计数累加器自带一把短临界区锁，一次创建在同一临界区内推进 `created_total`/`alive`/`peak_alive`，一次析构同时推进 `destroyed_total`/`alive`，快照也在同一临界区内取四字段——因此任意并发快照都自洽（`created_total - destroyed_total == alive` 且 `peak_alive >= alive`），不要求消费者自己拼装锁协议。临界区只做整数运算，不调用用户代码、不分配，也不发起协程等待；析构记账不依赖静态析构顺序。`CreateObjectInfo` 保留为低层兼容入口，R2/R3 不能用裸身份分配绕过 creator 提交。

观测覆盖与 Co-only 是两个维度：framework 的 `CoApp`、`CoService`、`CoComponent`、`CoActor` 及 infra Client/Server、native `Engine` 都接同一 creator/Member 实际析构通知。Engine 不因 native 域免观测，也不因此继承 `CoRuntimeOnly`；Connection/CoTcpConnection/receipt 等无独立 owner 的值视图不强制工厂化。进程寿命 runtime 内核沿既有原生创建入口接观测，存活基线单列，不把它们改为 Co-only 或在 main 返回时销毁。
### 12.4 接口、文件与测试映射

| 节点 | 允许写入的文件 | 固定入口/结果 | 直接测试映射 |
|---|---|---|---|
| 上下文 | `bbt/coroutine/coroutine.hpp`、`bbt/coroutine/coroutine.cc`、`bbt/coroutine/object/CoRuntimeOnly.hpp`、`bbt/coroutine/detail/LocalThread.{hpp,cc}` | `IsInCoroutineContext()`、`RequireCoroutineContext()`、`CoRuntimeOnly`；`GetLocalCoroutineId`/`GetLocalCoroutineStackSize` 在 native 线程零分配 | `tests/coroutine/Test_runtime_context.cc`，ctest `coroutine.runtime_context_no_create` / `coroutine.runtime_context_runtime` |
| 对象观测 | `bbt/coroutine/object/interface/ICoObject.hpp`、`bbt/coroutine/object/`、`tests/coroutine/`、`tests/CMakeLists.txt` | `MakeCoObject<T>(kind,name,creator)` 转发到同一 Manager creator；保留合法 owner/deleter；`GetCoObjectStats(kind)` | `tests/coroutine/Test_runtime_objects.cc`，ctest `coroutine.runtime_objects`；负向编译由 BBT_NEGATIVE/TU 检查 |
| 下游消费 | coroutine 不新增网络或业务类型；framework/infra 使用其公开入口 | 通过公开头和依赖 pin 消费，不把网络 owner、业务 registry 或物理 fd 责任下沉到 runtime | 下游构建与消费验证按对应发布检查执行 |

上述文件界定上下文与对象观测相关范围；不改 `AGENTS.md`、CI、配置、依赖 pin、共享安装目录或 Scheduler 停机模型。

### 12.5 多尺寸栈与 CoPool 路由契约

本节冻结 StackPool 与 CoPool 的语义。StackPool 的精确分桶、预算与异步交付已实现；CoPool 的尺寸路由、有界队列与自等待拒绝也已实现（`CoPoolOptions` + `Submit(StackRequest, …)`）。栈对象创建后地址与可用容量固定，运行中不扩栈、不换栈、不搬迁。

- `StackRequest` 的值类型与 Start 前消费入口见 §12.2；`usable_bytes` 是可用容量承诺而非提示，按 `NormalizeSize` 页对齐后与 `protect` 组成唯一桶键 `(U, protect)`；0 非法。任何策略都只用请求对应的精确桶，不允许以小栈顶替大请求，也不允许跨桶兜底。
- 策略是进程级 `StackPolicy`（`Strict` / `Auto`），由 §12.2 的 `RuntimeOptions.stacks` 在 `Start` 前一次性消费：`Strict` 只接受已配置尺寸类与各类硬容量；`Auto` 按真实 `StackRequest` 建立精确尺寸桶。`Auto` 不降低任务声明尺寸。
- 预算：`Strict` 每类有硬 `max_objects`，全池上限为各类之和；`Auto` 同时受全池对象上限与全池可用字节预算约束。`live / borrowed / idle / pending` 与已提交字节数分别可观察；`pending` 在预留时即计入对象与字节预算，worker/monitor 栈也计入。超预算返回明确拒绝，不排队等待无界内存。
- 异步交付唯一顺序：锁内预留 `pending` → 维护 lane（runtime 既有 native 维护执行流，非业务 Co worker）锁外分配 `Stack` → 提交为 `live+borrowed` 并生成 move-only `StackLease` → `Context` 接收 lease 并构造 fcontext → Coroutine 入队成功。任一步失败或取消都回滚 pending；已提交但未入队的 lease 归还池，不发布半初始化 Coroutine。业务 Co worker 上不得同步分配栈；Co 路径没有同步分配兜底，等待分配用既有 CoWaiter/awaitable 按调用方 deadline 收口。
- `Update`/缩容只回收 idle，不触碰 borrowed/pending；`Release` 按 `Stack` 自身的 `(U, protect)` 回桶，不读当前全局默认值。

实现边界与算法：

- 默认 Auto 的对象上限取启动前 `m_cfg_stackpool_max_alloc_size`，可用字节预算为 **256 MiB**，计入 `live + pending`，保护页不计入可用字节。显式 Strict 的全池对象上限就是各类之和，不再叠加旧默认上限；显式 Auto 使用两个显式上限。直接构造池的旧入口保留动态 `borrowed + pending` 兼容限制，并同时执行有限 Auto 内存预算。
- Auto 是按需补充，不预测尺寸、不做二次幂类或反馈控制：精确桶有 idle 就复用；没有则先检查对象/字节预算，压力下逐个回收 idle 直到可预留，否则立即拒绝。借出与预留对象不参与回收。已有平均用量采样仅影响定期 idle 缓存保留，不改变准入预算。
- 维护 lane 复用 Scheduler 原有 `_OnUpdate` 的 native 执行流，每拍最多处理一个 FIFO 分配请求。队列只含已预留请求，长度受同一对象预算限制；不建线程、executor 或独立分配平台。THREAD/LOOP 自动推进；NO_LOOP 必须由 native 驱动 `LoopOnce`，不驱动时只能等待、取消或到期，绝不在 Co 上同步分配兜底。
- `StackLease` 是带池归还器的 `unique_ptr`，请求记录共享延寿但栈不共享。Context 值接收 lease，成员构造、callable、fcontext 或描述失败均由唯一 owner 回收；Scheduler 只在完全构造后锁内入队，队列拒绝或抛出由局部 `unique_ptr<Coroutine>` 锁外销毁。任何可挂起的 Acquire 均在 global/map/admission 锁外。
- `Acquire(request, WaitOptions)` 使用既有 CoWaiter，取消/超时/事件登记失败以 `StackWaitError::Status()` 区分，实际分配故障保留原异常。未开始的取消移除队列并撤回 pending；已进入系统分配不能强制中断，保留 pending 字节到分配返回，随后销毁或归还晚到栈。维护流交付前检查绝对 deadline，不交付已经过期的分配。完成与协程取消仍遵循 Waiter 首胜仲裁。
- `Observe()` 在同一把池锁下读取 live/borrowed/idle/pending 和 committed/pending 可用字节；归还成功从 borrowed 转 idle，字节预算只有 idle 真正回收时才释放，不把缓存伪报为已释放内存。局部池必须活过全部 lease 和在途请求；全局池保持进程寿命。
- Scheduler 的 P worker 使用 pthread 原生栈，Start 不消耗 StackPool 栈；默认类必须至少容纳主任务一个栈。这里的 worker/monitor 计数指 CoPool 的真实 Co 栈，它们与主任务、普通 Co 使用同一 Acquire，不享有预算豁免；其尺寸和固定数量的创建前校验归 CoPool。
- 比此更简单的现成路径不足以闭合责任：原 StackPool 已有精确桶与锁外分配，却是同步裸指针交付；std/Boost 的栈分配器不处理本库 pending 预算、CoWaiter 取消和 Scheduler 入队。实现只补请求记录与 move-only lease，继续复用 Stack、池锁、CoWaiter 与 native 维护流。

`coroutine.stack_ownership_*` 覆盖 12 KiB/128 KiB × 两种保护属性、对象/字节预算、idle 压力回收与 borrowed 地址稳定、native/Co 分配故障、队列/Context/提交失败、pending/committed 取消、deadline 和单 worker 推进。测试先用维护流屏障证明阶段，再触发故障/取消，并核对调用次数及资源计数。CoPool 路由的要求与实现口径：

- `CoPool` 是固定 worker 的尺寸路由器：创建时每个 worker 类给定不可变 `StackRequest` 与固定 worker 数；`Submit` 携带 `StackRequest`，只路由到精确匹配的类。空类、满队列、停止后提交返回明确拒绝；不跨尺寸兜底、不无界排队、不为单个任务临时建栈。
- 活性：每类 worker 只执行本类任务，挂起任务只占用本类一个 worker，不承诺跨类借用。禁止同一 CoPool 内的等待依赖（任务 A 等待提交到同一池的任务 B）；这是使用前提，CoPool 不做死锁检测。HTTP 等模块的 writer/sequencer 必须使用独立类或独立池，不得占用 handler 类 worker。
- `Release`：封口拒绝新提交，唤醒排队者，已排队未执行的 future 以 broken_promise 兑现，等待 worker/monitor 协程退出；只允许在本池 worker 之外的域调用，池内 worker 调用是自等待编程错误，抛 `std::logic_error`，不得等待自身。Co 域调用不得占用本池 worker。
- 实现口径：`CoPoolClassConfig{request, worker_num, queue_capacity}` 在 `Create(CoPoolOptions)` 构造期校验（规范化键唯一、worker 数与队列容量非零），并用 `StackPool::CanAdmit` 在创建任何 worker 前核对各类 worker 栈与 monitor 默认栈的准入；不通过以 `std::runtime_error` 拒绝，创建中途失败回收已登记 worker 后才抛出，不留半成品池。`Submit(StackRequest, …)` 返回 `CoPoolSubmitStatus`，`NoExactClass` 覆盖未声明规格与未携带尺寸的提交（尺寸路由池没有默认类，旧 `Submit(callback)`/`SubmitWithFuture` 在这类池上直接拒绝）。每类队列容量固定，worker 数固定；任务异常沿用 future/回调/计数三条隔离路径。`Release` 的退出等待用 `m_created_workers`/`m_exited_workers` 账目：Co 域让出，native 域短睡，因此单 Processer 也能收口；自等待检查在封口之前。`coroutine.copool_router_*` 与 `coroutine.copool` 覆盖上述路径。
