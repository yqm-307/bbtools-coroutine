# bbtools-coroutine M1：核心运行时契约收敛与事件循环解耦计划

_Locked via grill — by Alice + 用户（2026-09-07）_

## Contract source

任务和依赖跟踪：[M1 里程碑](https://github.com/yqm-307/bbtools-coroutine/milestone/3)、[核心契约 #253](https://github.com/yqm-307/bbtools-coroutine/issues/253)、[EventLoop #255](https://github.com/yqm-307/bbtools-coroutine/issues/255)。仓内计划说明方向，不替代 GitHub 的实时任务状态。

本计划的核心语义以唯一契约真源 [`agent-docs/2026-09-07-core-runtime-contract.md`](./2026-09-07-core-runtime-contract.md) 为准。本文只记录 M1 范围、优先级和取舍，不复制契约正文；实现、测试和 Agent 消费文档发现冲突时，先更新差异与回归测试，再按契约收敛。

## Goal

确定 bbtools-coroutine 第一阶段的核心框架契约，使协程、调度器、事件等待、Hook 和异常行为成为后续可依赖的稳定边界；同时把 Chan、锁、条件变量、协程池等上层工具与核心解耦，允许后续独立迭代。目标不是立即实现所有能力，而是先固定语义、收敛职责、补齐关键契约测试，再以真实第三方客户端验证“同步代码尽量无损转为协程等待”。

## Approach

1. **冻结核心运行模型**
   - 继续使用有栈协程和当前轻量 `bbtco` 使用方式。
   - 保留 detached 协程，不在本阶段引入 `Task/Join` 作为强制模型；无人接收的异常必须按统一错误处理策略暴露。
   - 支持多 worker 并行；单个协程同一时刻只能在一个 worker 上运行，但挂起后允许迁移到其他 worker。
   - 继续使用全局单例运行时；`g_scheduler` 是当前阶段的核心设计边界，本阶段不做多实例。
   - 支持协作式取消：运行中不强杀；等待中的协程可被唤醒；取消必须经过正常 C++ 栈展开和 RAII 清理。

2. **确定核心职责边界**
   - 核心稳定概念：`Coroutine`、`Scheduler`、`EventLoop`、`CoroutineEvent`、`Timer`、`Hook Adapter`。
   - `Scheduler` 负责协程调度，不直接依赖具体 `Poller` 实现。
   - `EventLoop` 负责 FD、Timer、Wakeup 三类事件的驱动和交付。
   - `Poller` 只负责底层就绪事件注册、删除和等待，是可替换实现。
   - `CoroutineEvent` 负责一次等待的生命周期、状态和协程唤醒。
   - 第一版不扩展为多后端框架，只沿当前 Poller/协程事件边界做必要解耦。

3. **确定异常与运行时结果模型**
   - 用户任务内部允许使用 C++ 异常，利用栈展开和 RAII。
   - 运行时边界必须捕获异常，不能让异常逃出 `fcontext` 或 worker 执行边界。
   - 异常以 `exception_ptr` 保存并交付；提供任务结果/future 的工具 API 在取用时默认重新抛出，不要求核心新增 Task/Join。
   - 无接收者的 detached 任务异常必须日志 + 计数，禁止静默丢失。
   - Timeout、Cancelled、Closed、Rejected、SchedulerStopped 等正常运行时结果使用显式状态/返回值，不强制转换为异常。

4. **定义 Hook 兼容策略**
   - Hook 属于核心适配层，目标是透明兼容其他仓库的主流 POSIX 阻塞调用。
   - 对已覆盖调用，尽量保持返回值、`errno`、flags、超时、关闭和线程可见语义。
   - blocking FD、第三方库创建 FD、进程启动前创建 FD 都纳入兼容验证范围。
   - `MSG_DONTWAIT`、`SO_RCVTIMEO`、`SO_SNDTIMEO` 等语义不得被静默改成无限协程等待。
   - 无法安全转换时必须明确降级、返回原生错误或进入明确直通路径；禁止用“成功挂起”冒充兼容。
   - 兼容优先级按真实使用场景排序，不按 Hook 函数数量排序。

5. **保留并收敛用户入口**
   - 保留 `bbtco`、`bbtco_sleep`、`bbtco_yield` 等宏，降低已有代码迁移成本。
   - 稳定契约落在 C++ 类型和函数 API，宏只做薄封装，不承载额外生命周期语义。
   - 工具层继续提供便捷宏，但不得反向定义核心状态机。

6. **隔离工具层**
   - `Chan`、`CoMutex`、`CoRWMutex`、`CoCond`、`CoPool`、RAII guard 等属于工具层。
   - 工具层优先保持兼容，但允许重大变化；变化必须同步更新契约、测试和迁移说明。
   - 核心只提供通用等待、唤醒、超时、取消、关闭机制，不冻结具体同步工具的实现。
   - `CoPool::Release()` 等工具层生命周期语义单独定义，不污染 `Scheduler::Stop()`。

7. **补齐契约测试并做真实客户端验证**
   - 为协程生命周期、状态转换、协作式取消、异常交付和 detached 异常暴露增加契约测试。
   - 为 EventLoop 的 FD、Timer、Wakeup 路径增加独立测试，验证 Scheduler 不依赖具体 Poller。
   - 为 Hook 增加 blocking FD、`MSG_DONTWAIT`、socket timeout、EINTR、关闭和第三方 FD 场景测试。
   - 为 Stop 明确测试“停止接收、取消等待、worker 退出”，不把它误测为业务排空。
   - 用一个真实服务和真实客户端做长时间试运行，覆盖正常、超时、断连、重连、异常和停机。

## Key decisions & tradeoffs

- **Go 语义优先，C++ 差异显式定义。** 借鉴 Go 的协作式调度、channel 式等待和轻量并发体验；异常、RAII、future、线程和所有权按 C++ 习惯补充定义。
- **异常与状态混合。** 任务业务失败可以抛异常；超时、取消、关闭等预期运行时结果用状态。这样保留 C++ 可读性，又避免把常规 I/O 控制流全部变成异常。
- **Detached 保留。** 牺牲部分结构化并发的自动安全性，换取当前轻量 API 和迁移成本低。代价由统一异常处理、日志、计数和诊断弥补。
- **EventLoop 纳入核心边界，但不做过度抽象。** 拆分的目的是真正隔离 Scheduler 与 Poller，首版只覆盖 FD、Timer、Wakeup；不提前设计未知事件源和多后端插件体系。
- **全局单例暂不改变。** 接受 Hook/TLS/运行时全局绑定约束，避免第一阶段引入实例归属、跨实例 FD 和 API 迁移复杂度。
- **透明兼容优先，失败必须显式。** 兼容面可以逐步扩大，但不能用静默语义变化换取表面覆盖率。
- **工具层可变。** 核心契约稳定比同步工具 ABI/API 稳定更重要；工具层允许替换实现和修正错误语义，但需保留迁移信息。

## Risks / open questions

- blocking FD 的自动转换存在 POSIX 和第三方库边界，不能保证所有系统调用都能安全改写；必须按调用和客户端逐项实测。
- 有栈协程不能可靠拦截用户代码中的死循环；第一阶段只能提供 worker 无进展诊断和外部健康检查，不能承诺自动恢复。
- detached 协程没有天然的父子异常传播关系；无人接收异常的日志和计数必须成为强制运行时行为。
- 全局单例限制多 EventLoop、多租户和多组件隔离；只有真实需求出现后再单独设计多实例迁移。
- C++17 环境没有 `std::expected`，本阶段不新增第三方错误库；结果对象优先复用现有状态、future 和 `exception_ptr` 机制。
- 取消期间第三方阻塞调用可能无法立即中断；Hook 只承诺能管理自身事件等待，不承诺强制终止任意外部阻塞代码。

## Out of scope

- 本阶段不迁移到 C++20 原生 coroutine。
- 不强制引入 Task/Join 或完整结构化并发树。
- 不把 Chan、CoMutex、CoRWMutex、CoCond、CoPool 提升为核心稳定接口。
- 不新增多实例 Scheduler/EventLoop。
- 不一次性支持所有第三方框架、所有 libc 调用或所有文件类型。
- 不建设完整监控平台；只补核心快照、日志、计数和必要的无进展诊断。
- 不以继续增加 Hook 函数数量作为主要完成标准。
- 不在契约尚未确认前进行大规模实现重构。
