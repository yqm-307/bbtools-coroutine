## 背景

阶段 4 的输入前提已经成立：等待协议稳定、runtime 核心稳定、sync 原语稳定。现在需要收敛的是 Hook 与 `CoPool` 的行为一致性，以及它们对外暴露出来的中上层契约。

## 当前代码证据

- Hook 的 `connect`、`read`、`write`、`send`、`recv`、`accept`、`sleep` 都直接依赖 coroutine wait API，说明其对外语义会直接暴露底层等待稳定面的质量。
- `Test_hook.cc` 当前既有可复用的 `pipe` / 自建 listener 场景，也有依赖宿主 `ssh` 服务的 `t_hook_connect`，这说明 Hook 验证还没有完全 CI 化。
- `CoPool` 的两个 `Submit` 重载目前外部唤醒语义不一致，违反了 `coroutine-pool-and-api` spec 对等价调度行为的要求。
- `Release()` 当前通过 `NotifyAll()`、轮询和线程上下文分支完成 drain，shutdown 语义尚未被规格化。

## 目标 / 非目标

**目标：**

- 收敛 Hook 的 fd / timeout / retry 行为，使其 failure path、上下文前置条件和测试夹具都稳定。
- 收敛 `CoPool` 的 `Submit`、worker 唤醒、monitor 行为和 `Release` drain / shutdown 语义。
- 让 Hook 验证与 `CoPool` 验证脱离宿主环境偶然性。

**非目标：**

- 不修改基础等待完成模型。
- 不修改 scheduler / lifecycle / sync 的基础语义。
- 不直接收口 public API 和语法糖的最终对外说明。

## 设计决策

### 0. 阶段 4 不再回写 sync 中层语义

阶段 3 已经把以下语义冻结为阶段 4 输入，而不是可继续修改的实现细节：

- `CoCond` 的 expired waiter 跳过与 destroy/notify flush 规则。
- `CoMutex` 的 owner 规则与 timeout-aware `TryLock(int ms)` 结果语义。
- `Chan` 的 close、timeout、single-reader / rendezvous 与 state restore 语义。
- `CoRWMutex` 的 writer-preferred、公平性可观测性、非 owner unlock 防护与统一 waiter queue 规则。

如果阶段 4 的 Hook / `CoPool` 实现需要改变这些行为，应回退到阶段 3 change，而不是在本阶段直接补 sync。

### 1. Hook 验证必须优先清除宿主环境依赖

阶段 4 的 Hook 测试必须优先依赖 `pipe`、自建 listener 和确定性 timeout 场景，而不是宿主 `ssh`、固定服务端口或不受控外部服务。

### 2. CoPool 的 `Submit` 重载必须形成等价外部行为

后续实现应明确：两个 `Submit` 重载如果没有额外文档化差异，就必须对 worker 可见性和唤醒时机保持等价，而不是一个立即唤醒、一个等待 monitor 轮询副作用。

### 3. Release 必须成为显式 drain / shutdown 协议

`Release()` 不能再只是“尽量把 worker 叫醒然后等一会儿”，而应明确：

- 何时停止接受新任务
- 如何唤醒阻塞 worker
- 如何处理残留 work item
- 何时视为 drain 完成

## 建议测试文件拆分

- `unit_test/Test_hook_runtime_semantics.cc`
- `unit_test/Test_hook_runtime_timeout.cc`
- `unit_test/Test_copool_semantics.cc`
- `unit_test/Test_copool_shutdown.cc`

## 迁移计划

1. 先清理 Hook 和 `CoPool` 当前的环境依赖与行为漂移点。
2. 再收敛 Hook 的 retry / timeout / context 语义。
3. 再收敛 `CoPool` 的 `Submit` 等价性和 `Release` 协议。
4. 最后把这些稳定行为交给 public API / syntax 层消费。
