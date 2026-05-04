## 背景

当前仓库围绕 `bbt/coroutine` 构建了一个 Linux 平台的 Go 风格协程运行时，内部已经天然分成六个关注点：

- 调度与执行：`Scheduler`、`Processer`
- 协程状态与上下文：`Coroutine`、`Context`、`StackPool`
- 事件与阻塞适配：`CoPoller`、`CoPollEvent`、`Hook`
- 同步原语：`Chan`、`CoWaiter`、`CoCond`、`CoMutex`、`CoRWMutex`
- 协程池与用户入口：`CoPool`、`coroutine.hpp`、`SyntaxMacro.hpp`
- 测试与验证：`unit_test/`、`benchmark_test/`、`debug/`

当前问题不是模块不存在，而是行为契约没有被规格化。结果是代码中已经暴露出一些高风险区域，例如跨线程状态同步、等待对象生命周期、超时路径清理、停止路径资源回收、测试环境依赖与覆盖面不足。该设计文档的目标不是规定具体重构实现，而是定义一套可持续维护的规范组织方式，使后续 change 可以围绕单一模块或跨模块协议演进。

## 目标 / 非目标

**目标：**

- 建立一套与代码目录和运行时职责一致的模块化 specs。
- 每个模块 spec 仅约束该模块的职责、边界和对外可观察行为，不混入实现细节。
- 为所有模块建立统一测试准入标准，要求包含单元、集成、压力、回归与环境可重复性验证。
- 让后续 change 可以按模块扩展或修改 requirement，而不是反复写大而泛的总规范。

**非目标：**

- 本 change 不直接实现运行时代码修复。
- 本 change 不要求一次性改写现有所有测试。
- 本 change 不要求在 spec 内定义微观实现方案，例如具体锁实现、具体原子类型或数据结构选型。
- 本 change 不再作为阶段 1～5 的直接编码入口；这些实现阶段应在独立 change 中推进，并由本设计文档提供依赖顺序与验收边界。

## 当前定位

随着阶段 1～4 的实现被拆分到独立 change，这个 change 的职责已经收敛为两件事：

- 维护按模块拆分的 runtime specs，作为后续实现 change 的行为基线
- 维护跨阶段依赖关系、推荐拆分顺序与测试准入原则

因此，这里的“阶段 1～5”内容应被理解为路线图与依赖图，而不是要求在本 change 中顺序勾完的编码任务。

## 设计决策

### 1. 以运行时职责而不是目录名生硬映射来拆 capability

采用六个 capability：`runtime-scheduler`、`coroutine-lifecycle`、`event-hooking`、`sync-primitives`、`coroutine-pool-and-api`、`runtime-test-matrix`。

决策理由：

- 这六类能力能直接映射到当前代码结构与后续变更热点。
- 它们覆盖主要 cross-cutting 协议，但仍能保持单个 spec 可读。

备选方案：

- 只做一个总 spec。缺点是未来任一子模块调整都会污染整体文档，难以追踪 requirement 变化。
- 严格按目录拆，例如把 `StackPool`、`LocalThread` 单独拆 capability。缺点是粒度过细，早期维护成本高。

### 2. 将测试定义为独立 capability，而不是散落在各 spec 的末尾备注

采用独立 `runtime-test-matrix` spec，同时要求其他每个模块 spec 的场景都能映射到测试用例。

决策理由：

- “全面测试”是该 change 的硬约束，单独 capability 才能保证其成为实现准入条件。
- 独立测试 spec 能统一规范覆盖层次、环境约束、失败注入与回归要求，避免每个模块各写各的。

备选方案：

- 仅在 tasks 中加测试任务。缺点是任务可见，但没有稳定 requirement，后续 archive 时容易丢失测试标准。

### 3. 模块 spec 关注可观察行为与边界，不直接规定修复策略

每个 requirement 使用 SHALL/MUST 描述行为，并用场景覆盖正常、失败、竞争或超时路径。

决策理由：

- 当前库内已经存在多处实现争议点，先定义行为契约比先冻结技术选型更稳妥。
- 这样后续可以在 `design.md` 中针对具体变更做实现决策，而不必频繁改动基础 capability。

备选方案：

- 在 spec 中直接写出特定实现策略。缺点是规范会与实现细节耦合，后续优化会被误判为 requirement 变化。

### 4. 测试矩阵必须覆盖功能正确性、并发正确性、资源回收和环境可重复性

要求未来实现中至少按四层组织测试：

- 单元测试：验证模块内部状态机或局部语义
- 集成测试：验证模块间协作协议
- 压力/疲劳测试：验证高并发、高频切换与长期运行稳定性
- 回归测试：固化已知 bug、边界行为和过去事故

决策理由：

- 当前风险不只来自功能错误，更来自跨线程、生命周期和停机场景。
- 仅有 happy path 单元测试无法覆盖该类运行时问题。

备选方案：

- 只要求单测加少量 benchmark。缺点是无法证明停止路径、超时路径和竞争路径行为稳定。

## 模块细化：sync-primitives

本节作为 `sync-primitives` capability 的模块级设计约束，目标是为后续实现 change 提供统一的改造方向，而不是直接冻结某一个具体类的内部实现。

### 当前现状

当前同步原语层虽然名义上复用了 `CoWaiter` 与自定义事件机制，但实际上每类原语都维护了自己的一套等待与唤醒协议：

- `CoCond` 将栈上 waiter 地址直接放入队列
- `CoMutex` 直接保存 `CoPollEvent` 队列并自行触发
- `Chan` 混合维护 `m_is_reading`、读写 waiter 队列和 close 逻辑
- `CoRWMutex` 使用 `CoWaiter` 队列，但读写公平性策略只体现在局部逻辑中

这导致四类问题会反复出现：

- waiter 生命周期没有统一 owner
- timeout 和 notify 竞争时缺少统一清理协议
- 失败路径经常遗漏内部状态恢复
- 测试只能覆盖表面行为，难以验证竞争边界

### 代码证据

上述判断并不是抽象推测，而是当前实现已经显式暴露出来的接缝：

- `CoWaiter` 在 `Wait*` 路径里直接创建并持有 `CoPollEvent`，再通过当前 TLS 协程调用 `RegistCustom()`、`YieldWithCallback()` 和 `GetLastResumeEvent()` 决定返回结果。这说明 waiter 完成语义仍然直接建立在 coroutine/event 细节之上，而不是一个独立协议层。
- `CoCond` 在 `Wait()` / `WaitFor()` 中把栈上 `CoWaiter` 地址直接压入等待队列，`NotifyOne()` 再在另一条路径里回取并触发。只要 timeout、销毁或 notify 顺序发生竞争，就会立刻把 waiter 生命周期问题暴露出来。
- `CoMutex` 虽然提供 `TryLock(ms)`，但其等待路径当前通过 `_WaitUnLockUnitlTimeout()` 调用的是无 timeout 的 `RegistCustom()` 注册流程，也没有基于恢复原因做统一结果判定。这说明 timeout 语义还没有成为第一类协议。
- `Chan` 同时维护 `m_is_reading`、读写等待队列和 close 路径，并且多个失败分支会在尚未统一恢复内部状态的情况下提前返回。它不是一个单纯的“消息队列”问题，而是资源状态和等待状态混在一起的问题。
- `CoRWMutex` 使用 `CoWaiter` 队列，但写优先策略通过 `m_has_wait_wlock` 和局部 `_NotifyOne()` / `_NotifyAll()` 实现，公平性目前更多是代码细节而不是被规格化的策略。

这几处证据共同说明，`sync-primitives` 当前最大的风险并不是接口设计不够漂亮，而是等待生命周期、完成语义和资源状态恢复分散在多个类里各自处理。

### 目标结构

后续实现应将同步层拆成三层协议：

```text
┌──────────────────────────────────────┐
│ 同步原语接口层                       │
│ CoCond / Chan / CoMutex / CoRWMutex │
└──────────────────────────────────────┘
									│
									▼
┌──────────────────────────────────────┐
│ 统一等待协议层                       │
│ WaitNode / WaitQueue / Park-Unpark   │
│ - 注册等待                           │
│ - 取消等待                           │
│ - 超时完成                           │
│ - 唤醒完成                           │
└──────────────────────────────────────┘
									│
									▼
┌──────────────────────────────────────┐
│ runtime 事件桥接层                   │
│ CoWaiter / CoPollEvent / Coroutine   │
└──────────────────────────────────────┘
```

核心原则是：

- 同步原语不再直接决定 waiter 的裸生命周期
- timeout、notify、close、destroy 全部收敛到统一等待节点状态机
- 同步原语只表达资源条件，不重复实现等待协议

### 关键设计决策

#### A. 为所有同步原语引入统一等待节点抽象

后续实现应引入一个统一的等待节点抽象，可以命名为 `WaitNode`、`WaitHandle` 或等价名称，但它必须满足以下语义：

- 等待节点拥有明确生命周期，不依赖栈上裸地址被外部异步持有
- 节点状态至少区分：初始、等待中、已唤醒、已超时、已取消、已完成
- 唤醒路径和超时路径对同一节点只允许一次成功提交

这样做的原因是把“谁可以结束这次等待”从原语代码中抽出来，避免 `CoCond`、`Chan`、`CoMutex` 分别维护三套 race 处理逻辑。

#### B. 原语自身只负责资源条件，不负责等待生命周期收尾

每个同步原语只保留自己的资源语义：

- `Chan` 负责缓冲区、单读者约束、读写配对和 close 状态
- `CoCond` 负责等待队列与通知顺序
- `CoMutex` 负责锁所有权和 unlock 唤醒策略
- `CoRWMutex` 负责读写竞争策略和饥饿策略

等待是否过期、是否已被通知、是否需要从队列中剔除，应由统一等待协议处理。这样可以把“资源条件成立”和“等待完成”彻底分离。

#### C. 超时 API 必须是第一类语义，而不是普通等待的变种

当前实现中最危险的一点是部分 timeout API 只是“看起来有 timeout”，实质仍依赖普通等待路径。后续设计应将 timeout 视为和 notify 对等的完成来源。

这意味着每次等待完成都要产出明确结果：

- 成功获取资源
- 被超时完成
- 被关闭或销毁打断
- 因内部错误失败

同步原语不得再通过隐式副作用推导本次返回值。

#### D. close / destroy 必须被视为广播式终止事件

`Chan::Close()`、`CoCond` 析构、锁对象生命周期结束等场景，都不应只理解为“对象即将不可用”，而应视为对所有待处理 waiter 的终止事件广播。

因此需要统一规则：

- 关闭或销毁后，不允许继续留下待处理 waiter
- 所有待处理 waiter 必须被唤醒到一个明确定义的终止结果
- 原语关闭后的后续调用必须进入文档化的稳定错误语义，而不是落入旧状态残留

### 各原语的目标演进方向

#### 1. CoCond

`CoCond` 后续应从“持有 waiter 裸指针队列”演进为“持有等待节点句柄队列”。

设计要求：

- `Wait()` 与 `WaitFor()` 走同一套入队协议
- 超时返回时节点会自动从通知可见集合中失效
- `NotifyOne()` 在遇到失效节点时跳过并继续，而不是把失败当作终点
- `NotifyAll()` 对有效 waiter 做精确一次恢复

#### 2. Chan

`Chan` 是同步层里状态最复杂的对象，后续应把其内部状态拆为两部分：

- 资源状态：缓冲区、关闭状态、无缓冲配对状态、单读者约束
- 等待状态：读等待队列、写等待队列以及每次等待的完成结果

设计要求：

- 所有失败路径都必须恢复 `m_is_reading` 或其替代状态
- 无缓冲与有缓冲实现应共享等待协议，只在资源条件上分叉
- `Close()` 必须同时终止读等待与写等待
- `TryRead/ TryWrite` 的 timeout 版本必须在返回时保持内部状态闭合

#### 3. CoMutex

`CoMutex` 后续应不再把“事件对象队列”当作等待队列本身，而应通过统一等待节点表达锁等待。

设计要求：

- `Lock()` 与 `TryLock(ms)` 共享同一等待协议
- timeout 路径必须真实使用 timeout 语义
- unlock 后的唤醒策略需要文档化，是 FIFO、近似 FIFO 还是仅保证正确性
- owner 校验和重入拒绝逻辑要与等待协议分离

#### 4. CoRWMutex

`CoRWMutex` 当前已经最接近“资源条件 + waiter 队列”的模式，但还缺少被规格化的公平性策略。后续应把公平性定义成显式设计选择，而不是隐藏在代码细节里。

设计要求：

- 明确是否采用 writer-preferred、reader-preferred 或 bounded fairness
- 所有通知路径都围绕该策略进行
- 读写 waiter 的超时、取消和关闭语义与其他原语保持一致

### 迁移策略

`sync-primitives` 不适合一次性重写。建议后续 change 分三阶段推进：

1. 先抽出统一等待节点与完成结果模型，但不立刻改写所有原语外部接口。
2. 先迁移 `CoCond` 和 `CoMutex`，因为这两类最能验证 waiter 生命周期和 timeout 竞争语义。
3. 最后迁移 `Chan` 与 `CoRWMutex`，并统一 close、公平性与大规模竞争测试。

这样做的原因是 `Chan` 的状态最重，如果一开始就拿它作为第一阶段，会把等待协议重构和资源状态修复混在一起，难以收敛。

### sync-primitives 的专项测试设计

该模块后续实现必须至少补齐以下测试层：

- 单元测试：
	- waiter 超时后失效
	- notify 与 timeout 同时竞争时只完成一次
	- close 或 destroy 后所有 waiter 都得到稳定结果
- 集成测试：
	- `Chan` 与调度器协同时的读写唤醒
	- `CoMutex` / `CoRWMutex` 与协程切换时的加锁解锁
	- `CoCond` 与多个生产者/消费者场景
- 回归测试：
	- timeout 后再次 notify 不触碰失效 waiter
	- 失败路径后 reader/writer 状态可恢复
	- 带 timeout 的锁 API 不退化成普通等待
- 压力/疲劳测试：
	- 高并发下重复 wait/notify
	- 高频 close/open 生命周期替代场景
	- 大量读写竞争和长时间运行不出现卡死或泄漏

### 结果约束

后续任何针对 `sync-primitives` 的实现 design，都应至少回答四个问题：

1. waiter 由谁拥有，何时失效。
2. timeout、notify、close、destroy 谁能结束一次等待，以及如何保证只结束一次。
3. 原语内部资源状态在失败路径上如何恢复。
4. 哪些测试能证明上述三点已经成立。

### sync 的耦合判断

`sync` 不能被简单视为“可独立重构的小模块”。更准确的判断是：

- 它的对外语义边界相对稳定
- 它的内部实现与 runtime 内核深度耦合

当前代码中的耦合主要来自三类接缝：

```text
sync primitive
	↓
CoWaiter / CoPollEvent
	↓
g_bbt_tls_coroutine_co / YieldWithCallback / RegistCustom
	↓
Coroutine / CoPoller / Scheduler
```

从现状看：

- `CoWaiter` 直接持有 `CoPollEvent`，并直接调用当前协程的 `RegistCustom()`、`YieldWithCallback()` 与 `GetLastResumeEvent()`
- `CoCond` 直接把栈上 waiter 地址放进队列，等待对象生命周期与通知时机紧耦合
- `CoMutex` 直接维护 `CoPollEvent` 队列，并依赖 TLS 当前协程指针做 owner 校验与等待注册
- `Chan` 与 `CoRWMutex` 虽然表面上通过 `CoWaiter` 抽象等待，但本质仍建立在同一套 coroutine/event 细节之上

再往下看，runtime 内核本身也把这种耦合放大了：

- `Coroutine::RegistCustom()`、`YieldUntilFd*()` 和 `YieldWithCallback()` 会直接构造 `CoPollEvent`，并把事件完成统一回送到 `Coroutine::OnCoPollEvent()`。
- `Coroutine::OnCoPollEvent()` 在事件触发后直接通过 `g_scheduler->OnActiveCoroutine()` 把协程重新投回调度器，这意味着任何等待完成语义都会影响 scheduler 激活路径。
- `Context::YieldWithCallback()` 明确依赖“先 yield，再注册事件”的顺序保证，说明等待协议一旦调整，协程切换与事件注册顺序也必须同步验证。

因此，若先大幅重写 `sync`，但后续又调整了协程恢复、事件注册或完成语义，那么 `sync` 仍会发生明显返工。它不是“不受下层影响”，而是“非常受下层协议影响”。

但这并不意味着 `sync` 不适合作为早期重构目标。更准确的定位是：

- `sync` 适合在共享等待协议稳定后，作为第一批中层模块验证新协议
- `sync` 不适合在共享等待协议尚未收口前，独立承担第一轮大改

这也回答了“后续依赖模块改动了，sync 的修改是否会小”这个问题：

- 如果后续改动发生在 public API、示例或 pool 层，而等待协议和 runtime 桥接保持稳定，那么 `sync` 追加改动应当较小
- 如果后续改动仍落在 `Coroutine`、`CoPollEvent`、TLS 上下文或等待完成语义层，那么 `sync` 的追加改动不会小

所以，降低 `sync` 后续改动量的关键不是“尽早重写 sync”，而是“先把它依赖的内核接缝收敛成稳定协议”。

## 整库重构路线图

本库的重构不建议简单按“目录顺序”或“调用层级”推进，而应按稳定性依赖链推进：先冻结行为契约，再自下而上改造实现。原因是本库上层模块并非只是普通调用者，而是直接依赖下层运行时细节，例如当前协程 TLS、事件注册语义、唤醒时序与 stop 行为。如果不先冻结这些行为，后续每向上推进一层都可能重复返工。

### 总体策略

采用以下组合策略：

- 行为契约与验收标准：自上而下冻结
- 代码实现与共享协议：自下而上重构
- 风险最高且耦合最深的共享协议层优先，不先碰最终 public API

整体依赖链如下：

```text
spec / test matrix
	↓
等待协议 / 所有权模型 / 完成结果模型
	↓
Coroutine / Context / CoPollEvent / CoPoller
	↓
sync
	↓
Hook / CoPool
	↓
coroutine.hpp / syntax / examples / docs
```

### 阶段级测试闸门

重构顺序不只是代码顺序，还必须绑定测试闸门。否则阶段虽然结束，语义却可能已经漂移。建议每一阶段至少满足以下测试出口：

| 阶段 | 最低测试出口 | 作用 |
|------|--------------|------|
| 阶段 0 | spec 场景与现有测试映射表 | 防止后续重构失去回归目标 |
| 阶段 1 | 等待状态机单元测试 + notify/timeout 竞争回归测试 | 证明共享协议层可以稳定复用 |
| 阶段 2 | 协程挂起/恢复/取消/超时/teardown 集成测试 | 证明 runtime 核心桥接稳定 |
| 阶段 3 | 每个同步原语的单元、竞争、回归、疲劳测试 | 证明 `sync` 已脱离旧的裸生命周期耦合 |
| 阶段 4 | Hook I/O 超时与 `CoPool` release/submit 集成测试 | 证明中上层建立在稳定协议之上 |
| 阶段 5 | 公共 API 验收示例 + 文档一致性回归测试 | 证明对外行为与实现一致 |

原则上，不满足当前阶段的测试出口，就不应进入下一阶段。

### 阶段 0：冻结行为契约与回归边界

目标：

- 明确哪些行为必须保持兼容
- 明确哪些变化会被视为 breaking change
- 为后续每一阶段提供回归测试目标

本阶段工作：

- 完成模块化 specs 与测试矩阵
- 为高风险模块补充模块级 design 细化
- 梳理现有测试与 spec 场景之间的映射关系

阶段完成判据：

- 每个核心模块都有对应 spec
- `runtime-test-matrix` 能覆盖后续所有重构阶段
- 任何实现前都能明确引用其对应 capability 与场景

说明：

当前 change 已经基本完成本阶段工作。

### 阶段 1：抽出共享协议层

目标：

- 把当前散落在多个模块中的等待、唤醒、超时、取消与终止语义统一起来
- 把“谁拥有等待对象”和“谁能完成一次等待”变成稳定协议

本阶段工作：

- 抽出统一的等待节点抽象
- 定义一次等待的完成结果模型
- 统一 close / timeout / notify / cancel / destroy 的完成语义
- 减少业务模块直接依赖 `g_bbt_tls_coroutine_co`、`RegistCustom()`、`YieldWithCallback()` 等底层细节
- 盘点并收敛第一批直接 consumer：`CoWaiter`、`CoMutex`、Hook 路径与 `bbtco_wait_for` 语法糖路径
- 为阶段 1 明确独立测试文件与测试用例切分，而不是继续把协议验证混在现有模块行为测试中

阶段完成判据：

- 新的等待协议可以被 `sync`、`Hook`、其他 await 逻辑共同复用
- timeout 与 notify 竞争可通过统一状态机证明只完成一次
- 生命周期由协议层拥有，而不是由各业务类局部拼装
- 至少存在一组可映射到独立测试文件的阶段 1 测试资产，覆盖状态机、竞争完成、waiter 生命周期、sync 桥接、fd 桥接与压力测试六类验证

阶段 1 的交付物应被视为明确的 change 资产，而不只是抽象方向：

- `extract-runtime-wait-protocol` proposal / design / spec / tasks
- 一组桥接稳定面定义，覆盖 custom wait、custom wait + timeout、fd wait + optional timeout
- 一组建议新增测试文件基线：`Test_wait_protocol_state.cc`、`Test_wait_protocol_race.cc`、`Test_wait_protocol_waiter_lifetime.cc`、`Test_wait_protocol_bridge_sync.cc`、`Test_wait_protocol_bridge_fd.cc`、`Test_wait_protocol_stress.cc`

为什么先做这一层：

- 这是后续所有中层模块共享的依赖面
- 如果跳过这一层先改 `sync` 或 `Hook`，后续核心协议变化会导致这些模块二次返工

### 阶段 2：重构运行时核心

目标：

- 稳定协程恢复、挂起、事件注册与完成的核心语义
- 统一 runtime 对共享协议层的接入方式

本阶段工作：

- 收敛 `Coroutine`、`Context`、`CoPollEvent`、`CoPoller` 的生命周期与边界
- 明确协程终态、事件终态与异常传播的处理顺序
- 收敛等待协议与 runtime 核心的桥接点

阶段完成判据：

- `Coroutine` 不再作为多个模块隐式共享状态机的临时拼接点
- 事件对象与协程对象的拥有关系清晰
- 核心路径具备覆盖挂起、恢复、超时、取消、异常与 teardown 的测试

为什么这一步先于 `sync`：

- `sync` 当前对 `Coroutine`、`CoPollEvent`、TLS 与事件完成语义强耦合
- 核心不稳时，`sync` 的任何重构都只能建立在旧耦合之上
- 阶段 2 应只消费阶段 1 已冻结的桥接稳定面，而不应再回头修改阶段 1 的完成结果模型、基础状态机或测试出口定义

### 阶段 3：重构 sync

目标：

- 让 `sync` 从“直接依赖 runtime 细节”的模块，演进为“依赖稳定等待协议”的模块
- 统一 waiter 生命周期、超时语义、关闭语义与状态恢复

本阶段工作：

- 迁移 `CoCond`
- 迁移 `CoMutex`
- 迁移 `Chan`
- 迁移 `CoRWMutex`

建议顺序：

1. `CoCond`
2. `CoMutex`
3. `Chan`
4. `CoRWMutex`

原因：

- `CoCond` 最适合验证 waiter 生命周期与 timeout 竞争
- `CoMutex` 能验证拥有者、超时与唤醒策略
- `Chan` 状态最复杂，应在共享协议已被前两者验证后再处理
- `CoRWMutex` 公平性问题适合在等待协议稳定后再显式化

阶段完成判据：

- `sync` 不再直接依赖裸 waiter 生命周期
- 失败路径可恢复状态在测试中被证明
- timeout 与 close 语义对所有原语保持一致风格

### 阶段 4：重构 Hook 与 CoPool

目标：

- 让阻塞 I/O 适配层和协程池建立在稳定的等待/恢复协议之上
- 去除当前依赖旧实现副作用的行为

本阶段工作：

- 收敛 Hook 的 connect/read/write/send/recv/accept 超时与错误语义
- 收敛 `CoPool` 的任务唤醒、阻塞等待与 release 语义
- 统一它们与 `sync` 以及 runtime 核心的交互协议

当前代码已经表明这一步不能再拖到“顺手修一下”的程度：

- `CoPool` 直接依赖 `CoCond` 作为 worker 阻塞/唤醒手段，因此它天然继承 `sync` 的 waiter 生命周期语义。
- 两个 `Submit` 重载当前外部语义并不一致，其中一个提交后会立即 `NotifyOne()`，另一个只入队不显式唤醒，这说明 pool 层已经出现了公共 API 语义漂移。
- `Release()` 同时依赖 `g_scheduler->IsRunning()`、`NotifyAll()` 和当前线程是否启用协程语义来选择 sleep 路径，表明 shutdown 行为跨越 pool、sync、scheduler 和 Hook 语义边界。

阶段完成判据：

- Hook 行为的失败路径、超时路径和上下文前置条件稳定
- `CoPool` 的提交、唤醒、释放语义不再依赖实现偶然性
- 相关测试不再依赖外部环境或固定宿主服务

为什么不在 `sync` 之前做：

- `CoPool` 直接依赖 `CoCond`
- Hook 直接依赖当前协程等待语义
- 这两层都建立在前述共享协议与 runtime 核心之上
- 如果阶段 4 仍需要修改 custom wait、fd wait 或 timeout 的基础完成语义，应回退视为阶段 1 未完成，而不是在 Hook / CoPool change 中继续临时修补

### 阶段 5：收敛公共 API 与语法层

目标：

- 把已经稳定的底层与中层行为通过更干净的 public API 暴露出来
- 只在最后阶段处理兼容包装、语法糖和文档出口

本阶段工作：

- 收敛 `coroutine.hpp` 暴露面
- 清理 `syntax/*` 中依赖旧内部细节的部分
- 更新 example、README 与对外使用说明

阶段完成判据：

- public API 建立在已稳定的实现和语义之上
- 语法糖不再掩盖关键前置条件与失败模式
- 示例、文档与行为契约一致

为什么最后做：

- 这层最不应该驱动底层协议变化
- 提前改 public API 会放大返工范围，并使中间阶段难以保持兼容

### 不推荐的顺序

以下顺序风险较高：

- 纯自上而下：先改 `coroutine.hpp`、`syntax`、示例，再逼底层适配
- 纯自下而上：在未冻结行为契约时直接重写底层实现
- 从 `sync` 直接开刀，但不先抽共享等待协议

这些顺序的问题分别是：

- 上层先改会把旧底层耦合向更大范围扩散
- 下层先改但没有契约，会让行为漂移难以及时发现
- 直接改 `sync` 会在后续 runtime 核心变化时高概率返工

### 每阶段的统一验收要求

每个阶段结束前都应回答以下问题：

1. 当前阶段稳定了哪一层协议。
2. 上一层模块是否可以在不理解内部细节的前提下依赖这一层。
3. 下一阶段是否只需要围绕当前稳定协议工作，而不是继续修改它。
4. 已有 spec 场景和测试矩阵是否能证明本阶段的语义没有漂移。

### 最终建议

对本库而言，最合适的顺序不是“自上而下”或“自下而上”二选一，而是：

- 先自上而下冻结行为、边界和测试目标
- 再自下而上按共享协议 → runtime 核心 → sync → Hook/CoPool → public API 的顺序推进

如果只能给一个执行口令，可以概括为：

**先定规矩，再改地基；地基稳了，再改中层；最后才动门面。**

## 推荐后续 Change 拆分

如果按方案 1 落地，后续 change 最适合按以下顺序拆开，而不是把整个库当作一个大重构任务处理：

1. `extract-runtime-wait-protocol`：先把共享等待协议、完成结果和桥接面收口。
2. `align-runtime-lifecycle-and-scheduler`：再收敛 `Coroutine`、`Context`、`CoPollEvent`、`Scheduler` 的生命周期与激活语义。
3. `migrate-sync-primitives-to-wait-protocol`：在稳定协议之上迁移 `CoCond`、`CoMutex`、`Chan`、`CoRWMutex`。
4. `stabilize-hook-and-copool-semantics`：统一 Hook I/O 与 `CoPool` 的提交、唤醒、release 语义。
5. `clean-public-api-and-syntax-surface`：最后处理 `coroutine.hpp`、`syntax`、example 和文档出口。

这样的拆法有两个好处：

- 每个 change 都有明确的主 capability，可直接复用当前模块化 specs。
- 每个 change 都能绑定一组更聚焦的测试出口，避免一次性引入过大的回归面。

这些 change 之间还应满足更明确的依赖关系：

- `align-runtime-lifecycle-and-scheduler` 依赖 `extract-runtime-wait-protocol` 已冻结桥接稳定面与阶段 1 测试出口。
- `migrate-sync-primitives-to-wait-protocol` 依赖前两个 change 已冻结等待完成语义、协程生命周期边界与事件桥接顺序。
- `stabilize-hook-and-copool-semantics` 依赖 `sync` 侧试点迁移结果已经证明 custom wait 与 fd wait 可以共享同一协议面。
- `clean-public-api-and-syntax-surface` 只能消费前述稳定行为，不应再反向推动底层协议变化。

换句话说，后续 change 的关系不是简单串行，而是“每一阶段只能建立在前一阶段已冻结的稳定面之上”。只要某个后续 change 仍在迫使前一阶段重写其基础语义，就说明重构顺序失控了。

## 风险 / 权衡

- [风险] 模块边界与未来重构方向不完全一致 → 缓解方式：规范聚焦职责与对外行为，不将目录层面的偶然结构写死。
- [风险] capability 数量增加后维护成本上升 → 缓解方式：仅保留当前最核心的六个能力域，不继续细分到过小粒度。
- [风险] 测试要求写得太强，后续实现负担增加 → 缓解方式：在 tasks 中按优先级拆分测试工作，但 requirement 仍保持完整，避免质量门槛倒退。
- [风险] 当前实现与新 spec 可能存在明显偏差 → 缓解方式：允许后续 change 通过 design 和 tasks 分阶段收敛，不在本 change 中直接要求代码立即达标。

## 迁移计划

1. 先引入本 change 的 proposal、design、specs、tasks，作为后续 runtime change 的参考基线。
2. 后续任何针对 runtime 的实现 change，优先引用对应模块 spec，而不是重新定义行为边界。
3. 对发现的高风险实现点，分模块提出独立 change，并引用 `runtime-test-matrix` 作为测试准入标准。
4. 当实现与规格基本对齐后，再考虑将稳定 capability 归档到 `openspec/specs/` 的长期基线。

## 当前拆分状态

- `extract-runtime-wait-protocol`：已拆分并实现
- `align-runtime-lifecycle-and-scheduler`：已拆分并实现
- `migrate-sync-primitives-to-wait-protocol`：已拆分并实现
- `stabilize-hook-and-copool-semantics`：已拆分并实现
- `clean-public-api-and-syntax-surface`：仍应作为独立后续 change 推进

这意味着本 change 当前不应该再直接承载实现任务，而应继续作为上层蓝图维护这些 capability、依赖关系与验收口径。

## 待确认问题

- 是否需要在后续 change 中把 `StackPool` 和 `LocalThread` 提升为独立 capability，还是继续归入 `coroutine-lifecycle` 与 `coroutine-pool-and-api`。
- 是否要为 Hook 覆盖范围单独建立 syscall 支持矩阵，列出每个 syscall 的错误语义和测试要求。
- 是否要把 benchmark/fatigue 结果阈值也纳入正式 requirement，还是先停留在任务级别。
- 是否要把 `CoPool` 两个 `Submit` 重载的可见行为差异单独视为 API 契约问题，并在 `coroutine-pool-and-api` 下补专门 requirement。
