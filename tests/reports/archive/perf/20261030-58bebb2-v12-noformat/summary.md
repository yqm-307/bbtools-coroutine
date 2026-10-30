# v12 同步原语细节优化与 noformat 基线采纳

日期：2026-10-30。结论绑定 Fair-F1 基线 `83cefa3`、Go `1.27.3` 和 v12 短矩阵；本文只归档结论，不包含 raw、完整时间序列或构建目录。

## 结论

- 用户决定将收益门由“中位吞吐必须提升 ≥20%”的硬拒绝条件改为**软门禁**：20% 是明显收益参考线；低于 20% 不自动拒绝，稳定、超出测量噪声的正向收益可以结合守卫、适用场景和实现代价采纳。
- 用户据此采纳 `noformat` 并加入新基线。采纳决定与性能门禁证据分开记录：独立核验发现主场景未在首个候选样本前预冻结，所以 v12 的正式主场景收益门仍是 **NO_CLAIM**；不能将其表述为预注册门禁通过。
- 新唯一基线：`perf/sync-opt-v9-fair-baseline`，commit `58bebb20a21b9c44e9f40f47d9bf18bce1f27d1c`。在 `83cefa3` Fair-F1 上新增两处日志关闭时跳过 `snprintf` 装配的改动。不推送、不合并；本地基线 commit 已创建，本文档尚未提交。
- `timing` 不采纳；callback 装配方向拒绝；one-shot rendezvous 停止。没有组合 noformat 与 timing。

## 本轮指标

v12 使用 base / candidate / Go 三臂同窗比较；每候选覆盖 4 个 case × 2 种 worker 数 × 3 轮 × 3 臂，共 72 格；各格正式窗 1500 ms，整矩阵串行约 109.5 秒。最终独立核验从基线与两个候选共 216 个样本、432 个 stdout/stderr 流重算；argv、退出码、hash、checksum、直方图、CPU、Jain/min-mean 和 zero-progress 账目闭合。

| 候选／场景 | Δops/s 中位 | 三轮方向 | ΔCPU/op 中位 | ΔP99 桶上界 | 公平／风险 | 判定 |
|---|---:|---|---:|---:|---|---|
| noformat：waiter 四形状 | +11.9%～+14.5% | 4/4 全部三轮同向 | −10.7%～−12.6% | −14.9%～−28.4% | Jain、min/mean 未触门；zero-progress=0；mutex/chan 守卫无触发 | **用户采纳** |
| timing：waiter 四形状 | −0.8%～+1.0% | 无三轮同向形状 | −1.1%～+0.8% | −9.0%～+3.6% | 守卫无触发；未显示可辨收益 | **HOLD，不纳入基线** |

P99 是 `log2-128-v1` 命中桶上界的变化，不是未分桶的精确延迟分位数。noformat 四形状候选/Go 中位比约为 `0.294x～0.347x`；未追平 Go。timing 的小于约 1% 变化低于本次矩阵分辨率，不推断为收益或退化。

## 候选与验证经过

1. **v9：Fair-F1 建立旧基线。** 从 `04da899` 改 Scheduler 本地唤醒协程入队，保留 global fallback 与通知；独立核验后用户明确接受为唯一性能基线，提交 `83cefa341c29329b28656756f44395cbb33688b3`。矩阵中 `chan-w1/base` zero-progress 无效格仍如实保留。
2. **v10：候选审核后停止。** `home worker` 缺跨 worker 直接计数；driver 空槽门控估计约 50–150 ns/op，且目标 waiter 路径可达性不足。放行候选为 0，无运行时改动。
3. **v11：只读调度诊断后停止。** 在 `pairs=8,w2` 约 371 万次唤醒中，`home_ne_local=0`、steal/drain/global fallback 均为 0；不支持重开 home 定向投递。探针不作为性能臂，无实现收益声明。
4. **v12 基线校准与候选审核。** 旧 A/A raw 回放 72/72、新 A/A 短矩阵 72/72；callback 探针记录每次 `WaitWithCallback` 有 1 次 `CoPollEvent`/control-block 分配、1 次 callback slot store、3 次 user-callable 深拷贝（0 move）。可安全减掉的最小复制收益不足以支撑 C 候选，且 callback 异常边界未闭合，故 C 为 REJECT；D one-shot 仅设计风险审查、未实现。
5. **A noformat。** 仅在 `BBT_COROUTINE_DEBUG_PRINT` 开启时执行 `Coroutine::YieldWithCallback` 与 `Coroutine::OnCoPollEvent` 两处格式化；日志开启行为保留。OFF/ON 定向构建与 CTest 各 12/12 通过，反汇编确认 OFF 两个目标函数不再调用 `snprintf`、ON 时均保留一次。合入 Fair 基线后聚焦 CTest 12/12 通过。
6. **B timing。** 在 `Processer.cc` 将 Resume 起始时间保存在局部变量，保留用于 Steal 的 `exchange` 与两次时钟读取；聚焦 CTest 14/14 通过。矩阵中端到端效应不可见，因此不采纳。
7. **候选对比与独立核验。** noformat、timing 各 72/72 有效；无失败格、无 INVALID、无 zero-progress、无风险守卫触发。独立重算确认样本、hash、argv 与各项指标；同时发现主场景预冻结缺失，故正式收益门记为 NO_CLAIM。用户随后明确决定采纳 noformat。

## 基线身份

- 分支：`perf/sync-opt-v9-fair-baseline`
- 新 HEAD：`58bebb20a21b9c44e9f40f47d9bf18bce1f27d1c`
- 新增提交：`perf(coroutine): 关闭日志时跳过yield/notify路径的snprintf装配`
- 相对前一基线的源码文件：`bbt/coroutine/detail/Coroutine.cc`
- noformat commit diff SHA-256：`c45872be07733097d5de939647c650273d1a5c7e3c2e2638224821e29d194b4e`
- `build-fair/lib/libbbt_coroutine.so` SHA-256：`428a6b3cfa635cefcccf31327dca834447fd3dc9471f0c5eea0015ffc8470a2f`
- 前一 Fair-F1 commit：`83cefa341c29329b28656756f44395cbb33688b3`

## 收口与限制

- 继续保留正确性、zero-progress、公平性和 P99/CPU 退化守卫；只有“低于 20% 自动淘汰”的收益硬线改为软门禁。
- `Coroutine::Yield()` 的第三处同型格式化点未纳入本轮授权和测量，不宣称已优化。
- 没有运行全量 CTest、CI、组合回归或 soak；不能据此称已完成合并/发布验收。
- timing worktree 的未提交 diff 保留为 HOLD 实验，不属于新基线。
- GitHub Issue #411 未关闭或修改；本报告不代表远端推送、PR 或合并状态。

## 参考

- 优化循环 v12：`sync-opt-loop-v12`（本地 OMH DAG 记录）
- v9 基线采纳核验：`sync-opt-loop-v9`（本地 OMH DAG 记录）
- 本地性能身份 manifest 保留在工作区 scratch，不随本摘要入仓；仓内可复核身份以 commit、源码 diff 和库 SHA 为准。
