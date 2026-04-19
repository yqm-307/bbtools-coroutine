## 背景

当前库把等待、唤醒、超时、取消和终止语义分散在 `CoWaiter`、`CoCond`、`Chan`、`CoMutex`、`CoRWMutex`、`Coroutine` 与 `CoPollEvent` 的局部实现中，导致生命周期边界不统一、timeout 与 notify 竞争难以证明正确、关闭路径容易留下陈旧状态。现在单独抽出共享等待协议层，是为了让后续 `sync`、`Hook` 和 runtime 核心重构建立在稳定协议上，而不是继续围绕现有实现细节打补丁。

当前代码里已经有几个足够直接的信号表明，这个问题不能再拖到 `sync` 重构阶段顺手处理：

- `CoWaiter` 直接依赖当前 TLS 协程和 `RegistCustom()`、`YieldWithCallback()`、`GetLastResumeEvent()` 这组原始接口来完成等待。
- `CoCond` 会把栈上 waiter 地址直接放进队列，等待 notify、timeout 与对象生命周期竞争时自行收口。
- `CoMutex::TryLock(int ms)` 对外暴露 timeout 语义，但底层等待路径并没有统一的 timeout-aware custom wait 桥接。
- `Chan` 与 `CoRWMutex` 也都在各自对象内部同时维护资源状态和等待状态。

这些现象说明当前缺的不是又一个同步原语实现技巧，而是一层统一的等待协议与桥接稳定面。

## 变更内容

- 引入阶段 1 的独立 change，定义统一等待协议层的行为边界和实施路线。
- 为一次等待的生命周期建立统一模型，覆盖注册、等待、唤醒、超时、取消、关闭和销毁。
- 为等待完成结果建立统一语义，避免各模块通过局部副作用自行推导返回值。
- 要求后续实现先建立一层稳定桥接面，使 `sync` 的自定义唤醒路径、带 timeout 的 custom wait，以及 Hook 形态的 fd 等待都能映射到统一协议，而不是继续直接依赖 `g_bbt_tls_coroutine_co`、`RegistCustom()`、`YieldWithCallback()` 等底层细节。
- 要求本阶段用少量试点接入验证协议可用性，而不是直接完成全部 `sync` 或 Hook 的最终迁移。
- 要求本阶段交付覆盖单元、集成、回归和压力场景的测试方案，为后续 runtime 核心与中层模块迁移提供回归基线。

## 为什么先做这一步

本 change 的顺序优先级高于大规模 `sync` 重构，原因不是它“更底层”这么简单，而是它恰好位于所有后续 change 的共享接缝上：

- 如果先改 `sync`，后面再调整 `Coroutine`、`CoPollEvent`、TLS 当前协程或等待完成语义，`sync` 高概率返工。
- 如果先改 Hook 或 `CoPool`，它们仍然会继续踩在现有不稳定的等待拼装路径上，只是把问题往更上层扩散。
- 只有先把等待生命周期、完成结果和桥接面收口，后续 runtime 核心、`sync`、Hook、`CoPool` 才能围绕同一组稳定契约推进。

因此，这个 change 的角色不是“又一个并行模块重构”，而是整个方案 1 中的阶段 1 地基工程。

## 本阶段明确不做什么

为了避免范围膨胀，本 change 明确不把以下内容作为阶段 1 的直接完成标准：

- 不在本 change 中完成 `Chan` 与 `CoRWMutex` 的最终实现重写。
- 不在本 change 中收口 `CoPool`、`coroutine.hpp`、`syntax` 的最终公共契约。
- 不要求本阶段一次性替换掉所有旧等待路径，只要求形成稳定桥接面，并用少量试点消费者验证其可用性。

这意味着阶段 1 的验收重点是：协议是否稳定、桥接面是否成立、测试出口是否完整，而不是消费者数量是否已经全部迁完。

## 能力域

### 新增 Capabilities
- `runtime-wait-protocol`: 定义统一等待协议层的等待节点、完成结果、竞争完成规则、关闭语义以及与 runtime 的桥接边界。

### 修改的 Capabilities

- 无。

## 影响范围

- 受影响代码：`bbt/coroutine/detail/Coroutine.*`、`bbt/coroutine/detail/CoPollEvent.*`、`bbt/coroutine/sync/*`、`bbt/coroutine/detail/Hook.*`、`bbt/coroutine/pool/CoPool.*`
- 受影响测试：`unit_test/Test_cond.cc`、`unit_test/Test_chan.cc`、`unit_test/Test_comutex.cc`、`unit_test/Test_co_rwmutex.cc`、`unit_test/Test_hook.cc` 以及后续新增的协议层专项测试
- 受影响流程：后续 runtime 核心、`sync` 与 Hook 的实现 change 应先引用本 change，并把本 change 视为阶段 1 协议基线而非最终模块迁移终点

## 后续衔接

当本 change 完成后，后续 change 的合理顺序应为：

1. 基于该协议层收敛 runtime 核心的生命周期与事件桥接。
2. 再迁移 `CoCond`、`CoMutex`、`Chan`、`CoRWMutex` 等 `sync` 消费者。
3. 再收敛 Hook、`CoPool` 与上层公共 API。

换句话说，本 change 解决的是“统一等待语义怎么表达”，不是“所有等待消费者最终长什么样”。
