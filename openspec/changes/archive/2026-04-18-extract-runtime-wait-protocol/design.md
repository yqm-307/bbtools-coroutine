## 背景

当前等待相关逻辑横跨多个层次：

- `Coroutine` 暴露 `YieldWithCallback()`、`RegistCustom()`、`GetLastResumeEvent()` 等接口
- `CoWaiter` 负责把自定义事件与协程挂起绑定起来
- `CoCond`、`Chan`、`CoMutex`、`CoRWMutex` 在各自内部维护等待队列和完成逻辑
- `Hook` 直接使用当前协程的等待接口进行 I/O 挂起

这些模块本质上都在解决同一个问题：一次等待如何开始、由谁完成、在超时/通知/关闭竞争下如何只完成一次，以及完成后如何清理状态。但当前库没有一层明确的共享协议来承载这件事，导致每个模块都局部解决一遍。

### 当前代码证据

当前实现已经给出了足够直接的反例，说明“等待协议尚未被收口”不是推测，而是现状：

- `CoWaiter` 的 `Wait()`、`WaitWithCallback()`、`WaitWithTimeout()`、`WaitWithTimeoutAndCallback()` 都会直接创建并持有 `CoPollEvent`，再通过当前 TLS 协程调用 `RegistCustom()`、`YieldWithCallback()` 和 `GetLastResumeEvent()` 来完成等待与判定结果。这说明等待消费者仍然直接依赖 coroutine/event 原始拼装接口。
- `CoCond` 会把栈上 `CoWaiter` 对象地址直接压入队列，`NotifyOne()` 再异步回取该地址并触发。这使 waiter 生命周期天然暴露在 timeout、notify、destroy 的竞争面前。
- `CoMutex::TryLock(int ms)` 虽然对外暴露了 timeout 语义，但底层 `_WaitUnLockUnitlTimeout()` 当前仍通过无 timeout 的 `RegistCustom()` 路径注册等待，没有形成真正的 timeout-aware bridge。这说明“带 timeout 的等待”在当前库里还不是被统一建模的第一类能力。
- `Chan` 同时维护资源状态和等待状态，例如 `m_is_reading`、读写 waiter 队列与 close 路径会交叉影响多个失败分支，说明协议层缺失已经开始污染对象内部资源语义。
- `CoRWMutex` 虽然通过 `CoWaiter` 管理读写等待，但 writer-preferred 行为目前由局部状态 `m_has_wait_wlock` 和 `_NotifyOne()` / `_NotifyAll()` 逻辑隐式决定，还没有独立于等待生命周期被明确表达。

这些证据共同指向同一个结论：阶段 1 的首要目标不是“改一个更好的 sync 实现”，而是先把等待生命周期、完成结果和桥接面从各业务类中抽出来。

### 当前原始接口入口清单

基于当前代码扫描，直接拼装 `Coroutine` / `CoPollEvent` 原始等待接口的 consumer 主要集中在以下几组：

| consumer | 当前直接依赖的原始接口 | 角色判断 | 阶段 1 处理建议 |
|----------|------------------------|----------|-----------------|
| `sync/CoWaiter.cc` | `RegistCustom()`、`YieldWithCallback()`、`GetLastResumeEvent()`、`CoPoller::NotifyCustomEvent()` | custom wait 的兼容桥接层，同时也是当前最核心的耦合放大点 | 优先收敛为 façade，让它内部委托统一协议层 |
| `sync/CoMutex.cc` | `RegistCustom()`、`YieldWithCallback()` | 直接拼装 custom wait；`TryLock(int ms)` 仍未形成真正的 timeout-aware custom wait | 作为试点 consumer 之一，在阶段 1 直接迁移到新桥接面 |
| `detail/Hook.cc` | `YieldUntilFdReadable()`、`YieldUntilFdWriteable()`、`YieldUntilTimeout()` | fd wait / sleep wait 的直接 consumer | 阶段 1 保留最小验证路径，不要求一次性重写全部 Hook |
| `syntax/_WaitForHelper.cc` | `YieldUntilFdEx()` | 语法糖层直接暴露 fd wait | 视为 public-facing consumer，需要在阶段 1 至少验证其可通过同一桥接面建模 |

与此同时，还有一批当前不直接拼装原始接口、但实际依赖上述 consumer 的间接入口：

- `CoCond` 通过栈上 `CoWaiter` 入队，把 waiter 生命周期风险暴露给通知路径。
- `Chan` 通过 `CoWaiter` 管理读写阻塞，但把等待状态与资源状态混在同一对象里。
- `CoRWMutex` 通过 `CoWaiter` 管理读写等待，公平性策略仍停留在局部实现层。

这份清单对阶段 1 的意义是：

- tasks 1.4 不需要再从全仓库零散摸索“有哪些入口”，已知第一批入口就是 `CoWaiter`、`CoMutex`、Hook 路径和 `_WaitForHelper`。
- 试点迁移优先级应该是 `CoWaiter` → `CoMutex` / `CoCond`，而不是先碰 `Chan` 或 `CoRWMutex`。
- Hook 与 syntax 层虽然不是第一批全面迁移对象，但必须在阶段 1 被纳入桥接面兼容验证范围，否则协议层无法证明自己真的覆盖了 sync 之外的 consumer。

## 目标 / 非目标

**目标：**

- 定义统一的等待节点和完成结果模型。
- 定义等待的开始、完成、取消、关闭与销毁语义。
- 为 `sync`、`Hook` 和 runtime 核心提供稳定依赖层，降低后续模块重构返工概率。
- 为该协议层建立专项测试矩阵，覆盖正确性、竞争、关闭和回归场景。

**非目标：**

- 本 change 不直接完成所有 `sync` 原语的最终重写。
- 本 change 不直接收敛所有 public API 与语法糖。
- 本 change 不要求一次性替换掉所有旧实现，只要求建立共享协议层和迁移入口。

## 设计决策

### 1. 用统一等待节点承载一次等待的生命周期

后续实现应抽象出统一等待节点，可命名为 `WaitNode`、`WaitHandle` 或等价名称，但其职责必须固定：

- 记录一次等待的唯一身份
- 记录当前等待状态
- 记录等待完成结果
- 提供可被 timeout、notify、close、cancel 竞争完成的原子语义

决策理由：

- 这样可以把 waiter 生命周期从 `CoCond`、`Chan`、`CoMutex` 的局部代码里抽出来。
- 后续模块只需要表达“资源条件”和“完成时机”，不需要再各自实现一套等待完成协议。

备选方案：

- 继续以 `CoWaiter` 为中心，在每个模块局部补更多状态清理。缺点是只能减轻局部 bug，不能消除协议分散问题。

### 2. 用统一完成结果代替隐式副作用判断

一次等待完成后，应产生明确结果，例如：

- 成功完成
- 超时完成
- 被关闭打断
- 被取消打断
- 内部错误失败

决策理由：

- 当前库大量依赖“某个标志位 + 某个事件 + 某个返回值”的组合来倒推出本次等待发生了什么。
- 统一结果模型可以显著降低失败路径清理复杂度，也便于测试。

备选方案：

- 继续让每个调用者自己用事件位和局部状态判断完成原因。缺点是不同模块会产生不一致的语义分叉。

### 3. 共享等待协议层位于 runtime 核心之上、业务模块之下

目标分层如下：

```text
Coroutine / CoPollEvent / CoPoller
            ↑
      runtime wait protocol
            ↑
sync / Hook / future await logic
```

决策理由：

- 协议层需要依赖 runtime 的挂起/恢复能力，因此不能放在 `sync` 之上完全独立。
- 但它又必须成为 `sync`、`Hook` 等中层模块的共同依赖，否则耦合无法真正降低。

备选方案：

- 直接把协议逻辑放进 `Coroutine`。缺点是会把 runtime 核心继续变成所有模块的隐式耦合中心。

### 4. 协议层必须暴露稳定桥接面，而不是让调用方继续直接操作 Coroutine 接口

从现有代码看，等待相关调用主要分成两条路径：

- `sync` 路径通过 `RegistCustom()` + `YieldWithCallback()` + `GetLastResumeEvent()` 组合使用自定义事件
- Hook 路径通过 `YieldUntilFdReadable()`、`YieldUntilFdWriteable()`、`YieldUntilTimeout()` 直接进入 fd/timeout 等待

这说明当前库里并不存在一个统一、稳定的等待协议入口。后续设计必须显式引入一层桥接稳定面，使协议使用方表达的是：

- 我要发起哪一种等待
- 我要观察哪一种完成结果
- 完成后如何清理资源条件或队列状态

而不是要求调用方自己拼装：

- `Coroutine::RegistCustom()`
- `Coroutine::YieldWithCallback()`
- `Coroutine::YieldUntilFd*()`
- `Coroutine::GetLastResumeEvent()`
- `CoPoller::NotifyCustomEvent()`

建议的桥接关系如下：

```text
protocol consumer
sync / hook-shaped wait / future await logic
      ↓
wait protocol bridge
      ↓
Coroutine suspend-resume primitives
CoPollEvent / CoPoller
```

决策理由：

- 这样才能把“协议稳定面”和“runtime 内部细节”区分开。
- 后续 runtime 核心重构时，可以只保证桥接面稳定，而不是让所有中层模块一起跟着改 `Coroutine` 细节。

备选方案：

- 继续允许调用方自由组合 `Coroutine` 与 `CoPollEvent` 接口。缺点是协议层名义上存在，实际上耦合面并没有缩小。

### 4.1 桥接面必须同时覆盖 custom wait 与 timeout-aware custom wait

阶段 1 的桥接面不能只覆盖“普通 custom wait”和“fd wait”两种极端路径，还必须显式覆盖“带 timeout 的 custom wait”。否则像 `CoMutex::TryLock(int ms)` 这类 API 仍然会停留在“表面有 timeout，底层没有统一 timeout 语义”的状态。

阶段 1 至少需要保证三种输入都能通过同一桥接稳定面表达：

- custom wait
- custom wait + timeout
- fd wait + optional timeout

决策理由：

- 这是当前 `sync` 侧最容易发生语义漂移的地方。
- 如果阶段 1 不把 timeout-aware custom wait 纳入桥接面，后续试点迁移得到的只是一个只覆盖 happy path 的半成品协议。

### 5. 阶段 1 先保留 CoWaiter 作为兼容 façade，而不是立即删除

`CoWaiter` 当前虽然问题很多，但它已经被多个同步原语和局部逻辑复用。阶段 1 更稳妥的策略不是立即移除它，而是先把它降格为兼容 façade 或适配器。

这意味着阶段 1 之后允许出现以下状态：

- 新的等待节点和完成结果模型已经存在
- `CoWaiter` 内部改为委托协议层完成等待
- 上层模块短期内仍可通过 `CoWaiter` 过渡接入

而不要求阶段 1 立刻做到：

- 所有同步原语都绕过 `CoWaiter`
- Hook 全面改写为新接口
- 旧等待代码一次性清空

决策理由：

- 这能把“建立协议稳定面”和“全面替换消费者”拆成两个风险更小的阶段。
- 也更符合总路线图中“阶段 1 稳定协议、阶段 3 再系统重构 sync、阶段 4 再重构 Hook”的顺序。
- 它还能让阶段 1 先验证 `CoCond`、`CoMutex` 这类试点消费者，而不必同时把 `Chan`、`CoRWMutex`、Hook 全部拖进来。

备选方案：

- 在阶段 1 同时删除 `CoWaiter` 并迁移所有消费者。缺点是变更面过大，定位问题时很难分辨是协议错误还是消费者接入错误。

### 6. 本阶段测试必须先于大规模迁移准备好

本阶段应先建立针对共享协议层的测试集合，再开始大规模迁移 `sync` 或 `Hook`。

决策理由：

- 等待协议是后续多个模块共同依赖的地基。
- 如果协议层没有先建立专项回归，后续迁移中任何小改动都会导致定位困难。

备选方案：

- 先写实现，最后补测试。缺点是协议 bug 往往是竞争类问题，晚补测试时很难知道是协议错误还是模块接入错误。

## 风险 / 权衡

- [风险] 抽象层级增加后，短期内理解成本上升 → 缓解方式：先只抽象等待生命周期和完成结果，不一次性引入过多泛化概念。
- [风险] 协议层设计过度，导致接入复杂 → 缓解方式：先以 `sync` 和 `Hook` 的共性需求为边界，不为暂时不存在的场景设计。
- [风险] 迁移期间新旧等待路径并存 → 缓解方式：分阶段迁移，并要求每个阶段都有回归测试证明旧行为未漂移。
- [风险] 共享协议层仍然泄漏过多 runtime 细节 → 缓解方式：设计时显式检查哪些接口是“协议稳定面”，哪些仍属于内部实现细节。

## 阶段 1 完成判据

阶段 1 不以“所有消费者都迁完”为完成标准，而以“协议稳定面已经形成并被试点验证”为完成标准。建议完成判据为：

- 至少存在一组统一的等待节点、等待状态和完成结果抽象。
- `sync` 侧至少有一到两个试点消费者通过协议层接入，而不是直接拼装 `Coroutine` 原始接口。
- 带 timeout 的 custom wait 已经通过桥接面表达，不再由各消费者各自组合 `RegistCustom()`、事件位和局部返回值推导结果。
- 存在一个 Hook 形态的 fd/timeout 等待验证路径，用来证明协议模型既能表达 custom wait，也能表达 fd wait，但不要求本阶段完成全部 syscall Hook 重写。
- `CoWaiter` 的过渡角色明确，要么成为 façade，要么被限定在兼容边界内，不再继续扩散新的语义。
- 测试出口至少覆盖：等待状态机、单次完成竞争、关闭/销毁清理，以及一条 hook-shaped integration 路径。

同时，以下内容应被明确视为“阶段 1 不直接收口”的下游问题：

- `Chan` 的完整资源状态重构
- `CoRWMutex` 的公平性策略最终定稿
- `CoPool` 与 public API 的对外行为收敛

这些内容会消费阶段 1 的协议成果，但不应反过来主导阶段 1 的抽象边界。

## 阶段 1 测试出口映射

下面的映射不是“最终测试实现清单”，而是当前阶段的测试准入基线。目的不是证明现有测试已经足够，而是明确：

- 哪些 requirement 场景已经有可复用资产
- 哪些场景只有模块级间接覆盖，仍缺协议级断言
- 哪些场景目前几乎没有对应测试，需要在阶段 1 新增

### 场景映射表

| requirement 场景 | 当前可复用测试资产 | 覆盖判断 | 阶段 1 动作 |
|------------------|--------------------|----------|-------------|
| Wait enters pending state | `unit_test/Test_cond.cc`、`unit_test/Test_poller.cc` | 部分覆盖 | 需要新增协议层状态机单测，直接断言 pending/armed 状态，而不是仅依赖最终行为 |
| Wait finishes exactly once | 无直接协议测试 | 明显缺口 | 新增竞争完成回归测试，覆盖 notify/timeout/close/cancel 并发竞争时仅一次成功完成 |
| Timeout completes a wait | `unit_test/Test_cond.cc`、`unit_test/Test_coevent.cc`、`unit_test/Test_hook.cc` | 部分覆盖 | 需要新增协议层结果模型测试，验证 timeout 是显式完成结果，而不是靠事件位反推 |
| Close interrupts a wait | `unit_test/Test_chan.cc` | 部分覆盖 | 需要新增协议层关闭/终止测试，区分 close 终止与普通失败返回 |
| Stale waiter is skipped after timeout | 无直接测试 | 明显缺口 | 新增 stale waiter 回归测试，覆盖 timeout 后再次 notify/scan 不访问失效 waiter |
| Destroy path flushes pending waiters | 无直接测试 | 明显缺口 | 新增 destroy/teardown 测试，覆盖队列中残留 waiter 的统一终止 |
| Sync primitive uses shared wait protocol | `unit_test/Test_cond.cc`、`unit_test/Test_comutex.cc`、`unit_test/Test_chan.cc`、`unit_test/Test_co_rwmutex.cc` | 间接覆盖 | 阶段 1 需要把 `CoCond`、`CoMutex` 试点迁移后补一组协议接入集成测试 |
| Hook path uses shared wait protocol | `unit_test/Test_hook.cc`、`unit_test/Test_coevent.cc` | 间接覆盖 | 阶段 1 需要最小 hook-shaped wait 集成测试，证明 fd/timeout 等待通过桥接面接入 |
| Sync consumer arms a custom wait through the bridge | 无直接测试 | 明显缺口 | 新增桥接面集成测试，禁止继续直接依赖 `RegistCustom()` + `YieldWithCallback()` 拼装 |
| Hook-shaped wait arms an fd wait through the bridge | 无直接测试 | 明显缺口 | 新增桥接面集成测试，禁止测试通过直接读 coroutine 底层事件状态来判断成功 |
| Regression test protects single-completion guarantee | 无直接测试 | 明显缺口 | 把阶段 1 中发现的任一重复完成缺陷固化成永久回归测试 |
| Stress test exercises repeated wait competition | `unit_test/Test_poller.cc`、`unit_test/Test_comutex.cc`、`unit_test/Test_chan.cc` | 部分覆盖 | 需要新增协议层专项压力测试，而不是仅依赖现有模块压力测试替代 |

### 现有资产解读

当前可复用测试资产大致可以分成四组：

```text
协议相关直接素材
├─ Test_cond.cc      -> custom wait / timeout 的早期行为素材
├─ Test_poller.cc    -> event 注册、timeout、取消、多线程注册素材
└─ Test_coevent.cc   -> 事件辅助器、timeout、fd 事件素材

sync 模块行为素材
├─ Test_comutex.cc
├─ Test_chan.cc
└─ Test_co_rwmutex.cc

hook / fd 等待素材
└─ Test_hook.cc

阶段外参考素材
├─ Test_coroutine.cc
├─ Test_copool.cc
└─ debug/*
```

这些资产的价值主要在于：

- 它们已经证明当前仓库具备 coroutine、poller、sync、hook 的基本测试夹具
- 阶段 1 不需要从零设计所有测试环境
- 但它们大多验证“模块行为是否大致可用”，还没有验证“共享等待协议是否是稳定语义层”

因此阶段 1 的新增测试重点不应继续堆砌更多模块 happy-path，而应优先补：

1. 协议状态机测试
2. 单次完成竞争测试
3. stale waiter / destroy 清理回归测试
4. 桥接面集成测试
5. 协议层专项压力测试

### 文件级测试映射清单

为了让阶段 1 的测试工作不再停留在“有这些测试文件”这一层，当前仓库里可直接复用或可局部改造的测试资产可以进一步细化为下表：

| 文件 | 当前可复用场景 | 更适合在阶段 1 承担的角色 | 当前局限 |
|------|----------------|----------------------------|----------|
| `unit_test/Test_cond.cc` | `CoWaiter` 基本 wait/notify、`WaitWithTimeout()` 时间到期、`CoCond::NotifyAll()` 多 waiter 唤醒 | custom wait 试点素材；可演化为 `CoWaiter` façade 与 `CoCond` 接入测试 | 主要断言最终行为，几乎不验证 stale waiter、destroy flush 与单次完成竞争 |
| `unit_test/Test_comutex.cc` | `Lock()`/`UnLock()` 并发正确性、`TryLock(1)` 在竞争下可用 | `CoMutex` 试点 consumer 素材；可补成 timeout-aware custom wait 集成测试 | 当前只验证“带 timeout 的接口能跑”，没有证明 timeout 完成原因和 waiter 清理正确 |
| `unit_test/Test_hook.cc` | `socket`/`accept`/`read`/`write`/`send`/`recv`/`sleep` Hook 行为，`bbtco_wait_for` 语法糖路径，以及基于 `readable|timeout` 的 wait-for timeout 验证 | Hook-shaped wait 与 syntax wait 的最小桥接验证素材 | 目前仍主要覆盖行为结果，尚未把 bridge 完成结果显式做成独立断言文件 |
| `unit_test/Test_coevent.cc` | timeout 事件、fd readable/writeable 事件、带 CoPool 的事件回调 | event/bridge 层参考素材；可支持 fd wait + timeout 的桥接验证 | 它主要验证事件辅助宏，不直接证明协议 consumer 是否脱离原始 coroutine 接口 |
| `unit_test/Test_poller.cc` | `CoPollEvent` timeout 注册、取消、多事件、多线程注册 | 协议层底座素材；适合支撑状态机、取消和压力测试夹具 | 当前不在 scheduler / coroutine consumer 语义层，不能替代桥接面集成测试 |
| `unit_test/Test_chan.cc` | buffered / unbuffered chan、close、block read/write、TryRead timeout | `Chan` 的阶段外参考素材；帮助标记后续协议迁移缺口 | 当前覆盖的是对象行为，不足以证明共享等待协议已稳定；很多场景仍依赖 `CoWaiter` 间接语义 |
| `unit_test/Test_co_rwmutex.cc` | 读锁共享、写锁阻塞、多协程读写竞争 | `CoRWMutex` 的阶段外参考素材；帮助标记公平性与等待协议缺口 | 没有 timeout、取消、close 语义覆盖，也没有把公平性显式做成契约测试 |
| `unit_test/Test_copool.cc` | `CoPool::Submit()` 基本吞吐和 coroutine 上下文可用性 | 阶段外参考素材；说明后续 `CoPool` 可以消费阶段 1 协议成果 | 当前不覆盖 release、阻塞 worker 唤醒和 shutdown 语义，不应视为阶段 1 准入资产 |

如果按照“阶段 1 最少新增、最大复用”的原则推进，更合理的文件级落点是：

- 以 `Test_cond.cc` 为基础，拆出或扩展 `CoWaiter` / `CoCond` 的协议接入测试。
- 以 `Test_comutex.cc` 为基础，补一组真正验证 timeout-aware custom wait 的测试，而不是只循环调用 `TryLock(1)`。
- 以 `Test_hook.cc` 中已经落地的 `pipe` / 自建 listener 场景为基础，继续扩展 hook-shaped wait 集成测试，避免重新引入宿主环境依赖。
- 以 `Test_poller.cc` 为基础，补协议层状态机、取消与高迭代压力夹具。

相应地，当前阶段 1 明显还缺以下“文件级空白”：

- 没有一个测试文件直接验证 notify-vs-timeout、close-vs-timeout、cancel-vs-notify 的单次完成竞争。
- 没有一个测试文件直接验证 stale waiter 跳过与 destroy/teardown flush。
- 没有一个测试文件直接验证“带 timeout 的 custom wait 通过桥接面表达，而不是调用方自行推导结果”。
- 没有一个测试文件把 syntax wait (`bbtco_wait_for`) 和底层 hook/fd wait 统一拉到同一桥接契约下验证。

### 建议测试文件拆分

考虑到当前 `unit_test/` 目录已经采用 `Test_*.cc` 的单文件可执行组织方式，阶段 1 更适合沿用相同命名约定，而不是把所有新增测试继续堆进现有文件。建议拆分如下：

| 建议文件 | 主要职责 | 建议优先级 |
|----------|----------|------------|
| `unit_test/Test_wait_protocol_state.cc` | 共享等待协议状态机测试，覆盖 pending、armed、completed、timed-out、closed、canceled | 最高 |
| `unit_test/Test_wait_protocol_race.cc` | 单次完成竞争测试，覆盖 notify-vs-timeout、close-vs-timeout、cancel-vs-notify | 最高 |
| `unit_test/Test_wait_protocol_waiter_lifetime.cc` | stale waiter 跳过、重复扫描安全性、destroy/teardown flush | 最高 |
| `unit_test/Test_wait_protocol_bridge_sync.cc` | `CoWaiter` façade、`CoCond`、`CoMutex` 试点接入桥接面的集成测试 | 高 |
| `unit_test/Test_wait_protocol_bridge_fd.cc` | Hook-shaped wait 与 `bbtco_wait_for` 的桥接验证，覆盖 fd-ready / timeout wait | 高 |
| `unit_test/Test_wait_protocol_stress.cc` | 高迭代竞争、重复完成、悬挂 waiter、卡死与泄漏暴露 | 高 |

如果阶段 1 想尽量减少新文件数量，也可以采用折中方案：

- 保留 `Test_cond.cc` 和 `Test_comutex.cc` 作为试点 consumer 文件。
- 新增 4 个协议专项文件：`Test_wait_protocol_state.cc`、`Test_wait_protocol_race.cc`、`Test_wait_protocol_waiter_lifetime.cc`、`Test_wait_protocol_bridge_fd.cc`。
- 把压力测试单独留在 `Test_wait_protocol_stress.cc`，避免与功能性回归混在一起。

不建议的做法是：

- 把所有阶段 1 测试继续堆进 `Test_cond.cc`。
- 继续把更多协议级断言都堆进 `Test_hook.cc`，而不拆出更聚焦的 bridge fd 专项文件。
- 让 `Test_chan.cc` 或 `Test_co_rwmutex.cc` 承担协议层正确性的主证明责任。

### 建议 test case 粒度

按当前 Boost.Test 的书写习惯，阶段 1 可以直接采用下列用例粒度：

`Test_wait_protocol_state.cc`

- `t_wait_protocol_pending_to_completed`
- `t_wait_protocol_timeout_result`
- `t_wait_protocol_close_result`
- `t_wait_protocol_cancel_result`

`Test_wait_protocol_race.cc`

- `t_notify_vs_timeout_complete_once`
- `t_close_vs_timeout_complete_once`
- `t_cancel_vs_notify_complete_once`

`Test_wait_protocol_waiter_lifetime.cc`

- `t_stale_waiter_skipped_after_timeout`
- `t_destroy_flushes_pending_waiters`
- `t_repeated_notify_does_not_touch_completed_waiter`

`Test_wait_protocol_bridge_sync.cc`

- `t_cowaiter_uses_bridge_surface`
- `t_cocond_wait_for_uses_bridge_surface`
- `t_comutex_trylock_timeout_uses_bridge_surface`

`Test_wait_protocol_bridge_fd.cc`

- `t_fd_wait_uses_bridge_surface`
- `t_fd_timeout_wait_uses_bridge_surface`
- `t_bbtco_wait_for_uses_bridge_surface`

`Test_wait_protocol_stress.cc`

- `t_wait_protocol_high_iteration_race`
- `t_wait_protocol_no_stale_waiter_under_repetition`
- `t_wait_protocol_no_deadlock_under_mixed_completion`

这些命名的目的不是提前冻结实现细节，而是给阶段 1 一个足够具体的测试切分基线：一旦进入实现 change，维护者应该能直接从 OpenSpec 产物映射到新建测试文件和 test case，而不是再重做测试设计。

### 建议的阶段 1 测试出口

若要判断阶段 1 是否可收口，建议至少满足以下出口：

- 一组协议状态机单元测试：覆盖 pending、completed、timed-out、closed、canceled 等终态。
- 一组竞争完成回归测试：覆盖 notify-vs-timeout、close-vs-timeout、cancel-vs-notify 的单次完成保证。
- 一组 waiter 有效性测试：覆盖 stale waiter 跳过与 destroy flush。
- 一组试点 sync 集成测试：至少覆盖迁移后的 `CoCond` 与 `CoMutex`。
- 一组 hook-shaped 集成测试：覆盖一条 fd-ready 或 timeout 等待通过桥接面接入。
- 一组协议层压力测试：高迭代下不出现重复完成、悬挂 waiter 或卡死。

只有当这六类出口都具备，阶段 1 才足以作为阶段 2 runtime 核心重构的稳定前提。

## 迁移计划

1. 先引入共享等待协议层的 spec、design 和 tasks，作为阶段 1 的实施基线。
2. 先清点当前所有直接拼装 `Coroutine` / `CoPollEvent` 原始接口的等待入口，尤其是 `CoWaiter`、`CoCond`、`CoMutex`、Hook 形态 wait 与其他 timeout 调用点。
3. 定义桥接稳定面，并明确哪些接口属于协议层、哪些仍属于 `Coroutine` / `CoPollEvent` 内部细节。
4. 先把 `CoWaiter` 收敛为桥接协议的 façade，再优先将 `CoCond` 和 `CoMutex` 作为试点接入新协议，验证 waiter 生命周期和 timeout-aware custom wait 语义。
5. 用一条 Hook 形态的 fd/timeout 等待验证路径证明协议可复用，但不在本 change 中完成全部 Hook 重写。
6. 为 `Chan`、`CoRWMutex`、runtime 核心、`CoPool` 与 public API 的后续迁移准备入口约束，而不是在本阶段一次性完成它们的最终实现。
7. 当协议稳定面和试点验证完成后，再进入后续阶段的 runtime 核心重构与大规模消费者迁移。

## 待确认问题

- 共享等待协议层应放在 `detail/` 下单独命名空间，还是先以内聚到 `sync` 的公共基础设施形式落地。
- `CoWaiter` 是演进为协议层 façade，还是被新的等待节点抽象替代。
- timeout 与 close 的优先级是否需要在所有原语中保持完全一致，还是允许少量模块级特例。
- Hook 侧在阶段 1 采用最小验证适配器，还是直接为 `YieldUntilFd*` 建立新的桥接包装层。
