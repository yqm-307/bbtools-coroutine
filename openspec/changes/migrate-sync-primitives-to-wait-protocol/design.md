## 背景

当阶段 1 冻结等待协议、阶段 2 冻结 runtime 核心后，`sync-primitives` 才第一次具备了真正可以系统迁移的条件。此时阶段 3 不再需要去发明新的等待模型，而是要把现有四类同步原语中混杂的资源语义、等待语义和失败路径清理开。

## 当前代码证据

### 1. CoCond 仍然把 waiter 生命周期暴露给外部竞争

`CoCond::Wait()` 和 `WaitFor()` 当前在栈上创建 `CoWaiter`，再把其地址直接放入队列，`NotifyOne()` / `NotifyAll()` 以后再异步回取。这意味着：

- timeout 后 waiter 是否已经失效，并不是由统一协议层拥有，而是依赖 `Notify()` 时能否碰巧识别出来。
- destroy 路径没有显式 flush 语义，只能通过日志侧面暴露“可能丢失协程”。

### 2. CoMutex 的 timeout 语义仍然是局部拼装产物

`CoMutex::_WaitUnLock()` 与 `_WaitUnLockUnitlTimeout()` 当前几乎是同一条逻辑，后者甚至没有真正使用 timeout-aware custom wait 注册路径。这意味着：

- `TryLock(int ms)` 目前更多是“带参数的等待接口”，不是已经被统一建模的 timeout 语义。
- owner 校验、unlock 唤醒与 wait 完成结果仍然混在同一对象内部收口。

### 3. Chan 把资源条件与等待完成耦合在一起

`Chan` 当前同时维护缓冲区、`m_is_reading`、读写 waiter 队列、close 状态和无缓冲 rendezvous 状态。其风险点集中在：

- 失败路径可能在 reading 状态尚未完全恢复时提前返回。
- close 同时影响 reader 和 writer，但没有一个统一完成结果模型对外表达“这是 close 终结而不是普通失败”。
- 无缓冲与有缓冲路径共享部分等待实现，但资源状态分叉较大，容易在迁移时相互污染。

### 4. CoRWMutex 的公平性仍然停留在隐式实现层

`CoRWMutex` 虽然已经通过 `CoWaiter` 管理等待，但 writer-preferred 行为主要由 `m_has_wait_wlock` 与 `_NotifyOne()` / `_NotifyAll()` 的局部组合体现。当前缺口在于：

- 公平性是代码细节，不是文档化策略。
- timeout、取消、关闭和公平性之间的组合语义还没有被统一建模。

## 目标 / 非目标

**目标：**

- 让 `CoCond`、`CoMutex`、`Chan`、`CoRWMutex` 统一依赖共享等待协议与稳定 runtime 核心。
- 把每个同步原语的资源条件与等待完成语义明确分离。
- 让 close、timeout、失败和 destroy 结果对各原语都形成稳定、可测的一致风格。
- 固定公平性、唤醒顺序和状态恢复的行为边界，使其进入文档化语义而不是停留在实现偶然性中。
- 为阶段 3 建立文件级测试切分，覆盖试点迁移、回归和压力场景。

**非目标：**

- 本 change 不扩张阶段 1 的 bridge surface。
- 本 change 不重新设计阶段 2 的 lifecycle / stop 协议。
- 本 change 不收口 Hook、`CoPool` 或 public API。

## 设计决策

### 1. 迁移顺序固定为 CoWaiter / CoCond / CoMutex 先行，Chan / CoRWMutex 后行

阶段 3 不适合把四类原语并行推进。更合适的顺序是：

1. 先把 `CoWaiter` 收敛为共享协议 façade
2. 再迁移 `CoCond`
3. 再迁移 `CoMutex`
4. 最后迁移 `Chan`
5. 最后迁移 `CoRWMutex`

决策理由：

- `CoCond` 最能验证 waiter 生命周期与通知语义。
- `CoMutex` 最能验证 timeout-aware custom wait 是否真的落地。
- `Chan` 资源状态最重，不适合作为第一批协议 consumer。
- `CoRWMutex` 的公平性问题应建立在等待协议和 close / timeout 语义已稳定的前提上。

### 2. 同步原语只表达资源条件，不再自己发明等待完成规则

阶段 3 的核心原则是：

- `CoCond` 只表达等待队列与 notify 语义
- `CoMutex` 只表达锁所有权和 unlock 唤醒策略
- `Chan` 只表达缓冲区、single-reader / rendezvous / close 条件
- `CoRWMutex` 只表达读写竞争和公平性策略

timeout、close、cancel、destroy 谁结束等待，以及如何只结束一次，应由共享等待协议和 runtime 核心统一保证。

### 3. close / destroy 必须被视为稳定的终止结果，而不是隐式失败返回

阶段 3 之后，同步原语不应再把 close / destroy 只当作“返回 -1 的一种情况”。至少在设计层面，需要把它们视为稳定的终止来源，使 reader / writer / waiter 能获得一致的结果语义。

### 4. CoRWMutex 的公平性必须进入文档化策略

阶段 3 不一定要立即决定最终是 strict writer-preferred、reader-preferred 还是 bounded fairness，但必须把公平性从“局部实现细节”提升为显式设计选择，并让测试围绕该选择组织。

### 5. 阶段 3 只消费前两个阶段稳定面

如果在迁移 `CoCond`、`CoMutex`、`Chan`、`CoRWMutex` 时仍需要修改 custom wait、fd wait、timeout 完成模型或 coroutine teardown 语义，本质上应回退判定为阶段 1 或阶段 2 未完成，而不是在阶段 3 里继续补基础设施。

## 建议测试文件拆分

参考现有 `unit_test/` 组织，阶段 3 更适合新增以下文件：

| 建议文件 | 主要职责 |
|----------|----------|
| `unit_test/Test_sync_waiter_lifetime.cc` | 覆盖 stale waiter、close/destroy flush、重复 notify 安全性 |
| `unit_test/Test_sync_cond_mutex_bridge.cc` | 覆盖 `CoCond`、`CoMutex` 试点迁移后的 bridge surface 接入 |
| `unit_test/Test_sync_chan_semantics.cc` | 覆盖 buffered / unbuffered chan 的 close、timeout、state restore |
| `unit_test/Test_sync_rwmutex_fairness.cc` | 覆盖读写竞争、公平性策略、timeout 与取消组合 |
| `unit_test/Test_sync_primitives_stress.cc` | 覆盖 wait/notify、channel 竞争、mutex / rwmutex 长时间压力 |

### 建议 test case 粒度

`Test_sync_waiter_lifetime.cc`

- `t_cond_waiter_timeout_is_neutralized`
- `t_cond_destroy_flushes_pending_waiters`
- `t_mutex_timeout_waiter_is_not_reused`

`Test_sync_cond_mutex_bridge.cc`

- `t_cocond_wait_for_uses_protocol_facade`
- `t_cocond_notify_all_skips_expired_waiters`
- `t_comutex_trylock_timeout_returns_protocol_result`

`Test_sync_chan_semantics.cc`

- `t_chan_read_timeout_restores_reader_state`
- `t_chan_close_unblocks_readers_and_writers`
- `t_unbuffered_chan_rendezvous_is_single_completion`

`Test_sync_rwmutex_fairness.cc`

- `t_rwmutex_writer_policy_is_observable`
- `t_rwmutex_timeout_does_not_leave_stale_waiter`
- `t_rwmutex_read_write_competition_respects_policy`

`Test_sync_primitives_stress.cc`

- `t_sync_wait_notify_stress`
- `t_chan_competition_stress`
- `t_mutex_rwmutex_long_run_stability`

## 迁移计划

1. 先确认阶段 1 和阶段 2 的桥接稳定面、生命周期规则和 stop 协议已经冻结。
2. 先把 `CoWaiter` 收敛为兼容 façade，再迁移 `CoCond` 与 `CoMutex`。
3. 在试点验证通过后，再迁移 `Chan` 与 `CoRWMutex`，并显式固定其 close / timeout / fairness 语义。
4. 当阶段 3 的 sync 稳定面成立后，再进入 Hook / CoPool 收敛阶段。

## 风险 / 权衡

- [风险] 如果 `Chan` 过早进入迁移，会把资源状态修复和等待协议迁移混成同一次高风险变更 → 缓解方式：坚持试点先行顺序。
- [风险] 若 CoRWMutex 公平性不被显式化，后续测试仍然只能验证“看起来能跑” → 缓解方式：把公平性策略提升为设计决策和测试出口。
- [风险] 若阶段 3 继续补基础等待语义，会打破前两个阶段的稳定面 → 缓解方式：任何基础协议回退都显式归类为前置阶段未完成。