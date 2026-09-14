# A4 Linux 生命周期切片验证摘要

## 结论

**状态：部分完成。** PR #341 已合入 `main`，收口 Linux 侧等待生命周期修复；该结果不代表 Issue #339 的三平台或真实第三方 adapter 验收完成。

## 交付

- 合并 PR：[#341](https://github.com/yqm-307/bbtools-coroutine/pull/341)
- 合并提交：[`0c0e54ad`](https://github.com/yqm-307/bbtools-coroutine/commit/0c0e54ad36fb3c2462d611cd7eeede0c517a0ddc)
- 功能提交：`5e68759fef14cb5d74825ee54d228af916dc92f2`
- 关联计划：[#339](https://github.com/yqm-307/bbtools-coroutine/issues/339)
- 文档整理：[#342](https://github.com/yqm-307/bbtools-coroutine/issues/342)

## 已验证结果

- PR 独立复评：无 Critical / Important 阻塞。
- PR CI“编译 & 单元测试”：`SUCCESS`。
- PR CI“性能回归检查”：`SUCCESS`。
- PR CI“Sanitizer 检查（ASAN+UBSAN）”：`SKIPPED`，不计为通过。
- 合并前 Linux 本地定向 Stop 回归：连续 5/5 轮通过。
- 合并前排除已知基线超时测试后的 CTest：38/38 通过。

CI 运行记录：[`34817101500`](https://github.com/yqm-307/bbtools-coroutine/actions/runs/34817101500)。本摘要只保留结论级信息；原始日志和构建产物不入仓。

## 本次范围

- CoWaiter / CoMutex 等待登记和取消预检；
- parked 协程的 Stop 回收和完成路径所有权；
- Stop / 重启代际竞争；
- custom 事件回调异常隔离；
- 事件状态和 Stop 竞态回归覆盖。

## 未覆盖或仍未完成

- 完整 CTest 中已知基线 `Test_hook_contract` 超时；本摘要不把它改写成通过。
- 远端 Sanitizer job 为 `SKIPPED`；本摘要不把本地 sanitizer 记录改写为远端 CI 通过。
- 跨 worker 恢复和用户 TLS 边界尚无确定性验收。
- Windows/MSVC、iOS 设备、iOS 模拟器尚无真实构建运行证据。
- 代表性真实第三方 adapter 尚未完成端到端验收。
- Issue #339 仍保持开放。

## 证据边界

本摘要来源于公开 Issue、PR、commit 和 CI 记录，以及合并前的本地验证记录。它只证明本摘要列出的局部结果，不证明未列出的平台、库、句柄或业务资源兼容性。