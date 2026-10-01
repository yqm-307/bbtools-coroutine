# Close／Stop 重构：coroutine 阶段候选验收摘要

## 范围

- 仓库：`yqm-307/bbtools-coroutine`
- 基线：`efcdab3da0a4957b1f3647d9e390bb6517f35c0e`
- 候选分支：`refactor/process-lifetime-b2bc245c1a48`
- 候选状态：未提交、未推送、未建 PR；本摘要不表示已发布。
- 实现范围：coroutine process-lifetime runtime、Scheduler/CoWaiter 公共面、CoEventValue、测试与直接消费者迁移。
- 明确未包含：infra/framework 下游迁移、依赖锁、CI、提交/推送/PR。

## 实际验证

父级在全新 `build/acceptance-r2-alice` 中执行：

- CMake configure：成功。
- 定向构建：`NEED_TEST=ON`、`NEED_DEBUG=ON`，56 条 `Built target` 输出，去重 30 个目标；`BUILD_EXIT=0`，构建日志无 warning/error。
- 定向 CTest：11 项，`CTEST_EXIT=0`，`100% tests passed, 0 tests failed out of 11`。
- 公共头与类型边界：公共头独立 TU、超容量/越界 ID/超深指针的负向 configure 校验、跨 DSO 类型识别均通过。
- r2 候选清单：120 个文件全部 SHA256 匹配，8 个删除项均不存在；父验收与独立复审期间无候选漂移。
- `git diff --check`：通过。

## 独立复核

未参与 r2 实现者的独立复审结论为：**条件提交**。

已核证的修复包括：CoEventValue 类型身份与深度边界、FD 回滚度量、进程退出寿命链、NO_LOOP/LOOP timer 驱动、并发 Notify 首胜、README/debug/core contract/DNS 直接迁移。

随后对 `managing-fatigue-tests/SKILL.md` 的文档修复完成独立只读复核；`agent-docs/api-reference.md` 的覆盖列表旧 Stop/IsRunning 自相矛盾也已最小修正。

## 未闭合与限制

- `AGENTS.md:17` 仍保留旧 Stop 表述；受保护文件写入被门禁拒绝，本次未绕过。`:68` 仍将 Stop 列为契约触发词，需后续受控窗口修正。
- cv 限定的中间指针层级缺少编译期正/负例，审查列为非阻塞未证项。
- 未执行全量 ctest、Windows/iOS、mysql/hiredis 示例或远端 CI。
- 本摘要是阶段性验证归档，不是 commit、PR、发布或整体三仓重构完成证明。

## 证据入口

- 父构建与测试原始日志：`build/acceptance-r2-alice/`（忽略目录，不入仓）。
- r2 实现摘要：任务 scratch 中的 `repair-r2-summary.md`。
- 独立审查：任务 scratch 中的 `independent-review-r2.md`。
- 候选 SHA256 清单：`build/repair-r2/r2-candidate-manifest.json`（忽略目录）。
