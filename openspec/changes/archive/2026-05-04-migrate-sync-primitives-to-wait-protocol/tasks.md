## 1. 前置条件确认

- [x] 1.1 确认 `extract-runtime-wait-protocol` 已冻结 custom wait、custom wait + timeout、fd wait + optional timeout 的 bridge surface。
- [x] 1.2 确认 `align-runtime-lifecycle-and-scheduler` 已冻结 coroutine 生命周期、stop 协议与 teardown 规则。
- [x] 1.3 明确阶段 3 只消费前两阶段稳定面，不回头修改其基础语义。

## 2. 试点迁移

- [x] 2.1 先将 `CoWaiter` 收敛为共享等待协议的 façade 或受控适配层。
- [x] 2.2 迁移 `CoCond`，验证 waiter 生命周期、expired waiter 跳过与 destroy flush。
- [x] 2.3 迁移 `CoMutex`，验证 owner、unlock 唤醒与 timeout-aware custom wait 结果语义。

## 3. 重对象迁移

- [x] 3.1 迁移 `Chan`，分离资源状态与等待状态，收敛 close、timeout 与 state restore 语义。
- [x] 3.2 完成 `CoRWMutex` 收口：把当前 writer-preferred 倾向提升为显式策略，固定 owner、唤醒顺序与 waiter 生命周期边界。
- [x] 3.3 在 `CoRWMutex` 收口后审计并清理 `sync/*` 残留的局部等待拼装逻辑，避免新旧协议语义继续并存。

## 4. 文件级测试出口

- [x] 4.1 基于现有 `Test_cond.cc`、`Test_comutex.cc`、`Test_chan.cc`、`Test_co_rwmutex.cc` 整理阶段 3 的可复用测试资产与缺口。
- [x] 4.2 新增 `Test_sync_waiter_lifetime.cc`，覆盖 stale waiter、destroy flush 与重复 notify 安全性。
- [x] 4.3 新增 `Test_sync_cond_mutex_bridge.cc`，覆盖 `CoCond` 与 `CoMutex` 的 bridge surface 接入。
- [x] 4.4 新增 `Test_sync_chan_semantics.cc`，覆盖 buffered / unbuffered chan 的 close、timeout 与状态恢复。
- [x] 4.5 完成 `Test_sync_rwmutex_fairness.cc`，在现有 writer-preferred / non-owner unlock 用例基础上补齐 waiter 清理、长竞争与策略可观测性。
- [x] 4.6 新增 `Test_sync_primitives_stress.cc`，覆盖 wait/notify、channel 竞争和 mutex / rwmutex 长时间压力。

## 5. 验收与衔接

- [x] 5.1 验证 `sync-primitives` spec 的 requirement 场景都已映射到具体测试出口。
- [x] 5.2 以当前阶段 3 产物重新审视 `stabilize-hook-and-copool-semantics` 的依赖边界，确认 Hook / CoPool 不再需要改动 sync 基础语义。
- [x] 5.3 为阶段 4 只消费 sync 中层稳定面的后续计划补齐记录；若沿用现有 change，则至少在相关 artifacts 中冻结边界说明。

> 当前重规划结论：`CoWaiter` / `CoCond` / `CoMutex` / `Chan` 与 4 个专项测试出口已基本落地，后续任务只围绕 `CoRWMutex` 收口、旧等待逻辑清理和阶段 4 衔接展开。
