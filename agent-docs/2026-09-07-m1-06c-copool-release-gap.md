# M1-06c CoPool Release 与工具层边界：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md` §6（工具层停止语义独立定义，不反向改变 Scheduler::Stop）
Issue：#281

## 已锁住

- `CoPool::Release()` 完整语义（头文件注释同步更新）：
  1. 停止接收：`Submit*` → -1 / 空 future（基线已有，测试锁定）；
  2. 等待 worker 协程与 monitor 协程退出（NotifyAll + latch，基线）；
  3. **取消式排空（本次修复）**：worker 全部退出后 drain `m_works_queue`，滞留
     任务不执行；带 future 的任务经 `delete Work` 释放 promise，future 以
     `broken_promise` 兑现。修复前滞留 future 永挂（RED：`wait_for(2s)` 不
     ready）。
- 幂等：重复 Release 第二次 drain 队列为空，安全（沿用 t_double_release_safe）。
- drain 放在 worker 退出之后：与 `_WorkCo` 消费路径无竞态，每个 Work 恰好
  处理一次（执行或被 drain）。
- 工具层边界：CoPool 停止语义不触碰 `Scheduler::Stop` 契约；Scheduler 已停
  时 Release 的等待段跳过（协程必死），drain 仍执行、不挂死。

## 已知限制

- 正在执行的长任务不会被 Release 打断（取消式停机对任务粒度同上：等其自然
  结束）；任务内若持有池外共享状态需自行保证停机安全。
- `broken_promise` 是 C++ 标准对"承诺未兑现即销毁"的既定错误，不是池的
  失败信号；需要"完成保证"的调用方应在 Release 前自行排干（等自建的
  done-latch）。

## 验证

- `Test_copool` 新增 `t_release_drains_pending_futures`（1 worker 占住 + 3 个
  排队 future：Release 后全部 ready 且 broken_promise）；修复前 RED，修复后
  全套 15/15 绿。
- 全量 `ctest -j2` 25/25（含 #280 停机契约与 Hook 系列）。
