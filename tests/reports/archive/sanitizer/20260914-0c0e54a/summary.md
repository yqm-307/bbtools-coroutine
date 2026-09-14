# A4 Sanitizer 验证摘要

## 结论

**状态：未形成完整 Sanitizer 通过结论。** PR #341 的远端 Sanitizer job 为 `SKIPPED`；本地定向运行只作为补充证据，不能替代远端 job，也不能证明不存在内存泄漏。

## 已记录结果

- PR：[ #341 ](https://github.com/yqm-307/bbtools-coroutine/pull/341)
- 合并提交：[`0c0e54ad`](https://github.com/yqm-307/bbtools-coroutine/commit/0c0e54ad36fb3c2462d611cd7eeede0c517a0ddc)
- 远端 CI：Sanitizer 检查（ASAN+UBSAN）为 `SKIPPED`。
- 合并前本地定向 sanitizer 记录：17 个生命周期相关用例通过；运行时关闭 leak 检测，结果不作无泄漏证明。
- 另行 smoke 对照中，当前树与干净基线的 LSan 聚合结果一致；该对照只能说明本次运行未观察到新增差异，不能替代完整泄漏归因。

CI 运行记录：[`34817101500`](https://github.com/yqm-307/bbtools-coroutine/actions/runs/34817101500)。原始输出、完整堆栈和构建树不入仓。

## 未覆盖边界

- 没有远端 ASan/UBSan 通过结果；`SKIPPED` 保持原状态。
- 没有把 leak detection 关闭时的运行称为无泄漏。
- 没有覆盖 Windows/MSVC、iOS 设备或 iOS 模拟器。
- 没有覆盖真实第三方 adapter 的端到端资源生命周期。
- 该摘要不改变 Issue #339 的整体未完成状态。
