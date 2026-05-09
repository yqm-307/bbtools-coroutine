## 1. 前置条件确认

- [ ] 1.1 确认 `extract-runtime-wait-protocol` 已冻结 custom wait、custom wait + timeout、fd wait + optional timeout 的 bridge surface。
- [ ] 1.2 确认 `align-runtime-lifecycle-and-scheduler` 已冻结 coroutine 生命周期、stop 协议与 teardown 规则。
- [ ] 1.3 明确阶段 3 只消费前两阶段稳定面，不回头修改其基础语义。

## 2. 试点迁移

- [ ] 2.1 先将 `CoWaiter` 收敛为共享等待协议的 façade 或受控适配层。
- [ ] 2.2 迁移 `CoCond`，验证 waiter 生命周期、expired waiter 跳过与 destroy flush。
- [ ] 2.3 迁移 `CoMutex`，验证 owner、unlock 唤醒与 timeout-aware custom wait 结果语义。

## 3. 重对象迁移

- [ ] 3.1 迁移 `Chan`，分离资源状态与等待状态，收敛 close、timeout 与 state restore 语义。
- [ ] 3.2 迁移 `CoRWMutex`，显式定义公平性策略，并统一读写 waiter 的 timeout / cancel / close 语义。
- [ ] 3.3 清理旧的局部等待拼装逻辑，避免新旧协议语义继续并存。

## 4. 文件级测试出口

- [ ] 4.1 基于现有 `Test_cond.cc`、`Test_comutex.cc`、`Test_chan.cc`、`Test_co_rwmutex.cc` 整理阶段 3 的可复用测试资产与缺口。
- [ ] 4.2 新增 `Test_sync_waiter_lifetime.cc`，覆盖 stale waiter、destroy flush 与重复 notify 安全性。
- [ ] 4.3 新增 `Test_sync_cond_mutex_bridge.cc`，覆盖 `CoCond` 与 `CoMutex` 的 bridge surface 接入。
- [ ] 4.4 新增 `Test_sync_chan_semantics.cc`，覆盖 buffered / unbuffered chan 的 close、timeout 与状态恢复。
- [ ] 4.5 新增 `Test_sync_rwmutex_fairness.cc`，覆盖公平性策略、timeout 与取消语义。
- [ ] 4.6 新增 `Test_sync_primitives_stress.cc`，覆盖 wait/notify、channel 竞争和 mutex / rwmutex 长时间压力。

## 5. 验收与衔接

- [ ] 5.1 验证 `sync-primitives` spec 的 requirement 场景都已映射到具体测试出口。
- [ ] 5.2 确认阶段 3 的实现产物可以作为 `stabilize-hook-and-copool-semantics` 的稳定依赖层。
- [ ] 5.3 为阶段 4 Hook / CoPool 收敛准备后续 change，确保其只消费 sync 中层稳定面。