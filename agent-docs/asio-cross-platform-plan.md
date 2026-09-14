# Asio 跨平台长程方案

**状态：** Issue #339 仍在推进；当前文档记录范围、责任边界和可核对状态，不替代核心运行时契约，也不把局部 Linux 结果写成三平台完成。

## 1. 当前范围

Issue #339 已确认采用“事件适配能力 + 代表性真实第三方 adapter 验收”作为交付口径，目标平台为 Linux、Windows 和 iOS。

范围约束：

- 不新增自有 TCP、UDP、HTTP 网络 API。
- 不内置第三方网络库驱动。
- 第三方库通过用户编写的 adapter，使用已声明的事件等待或 custom event 接入协程。
- Linux Hook 保留为平台专属能力；不承诺 Windows 或 iOS 提供透明 Hook 等价物。
- 不把 POSIX `int fd`、Windows `SOCKET` 和其他平台句柄互相等同。
- 运行时负责自身等待、唤醒、取消竞争和对象安全；第三方库与 adapter 负责网络操作、操作取消、socket、请求对象和业务缓冲区生命周期。

公开决策与阶段记录：

- 范围决策与 A4 覆盖核销：[#339](https://github.com/yqm-307/bbtools-coroutine/issues/339)
- 文档和报告整理任务：[#342](https://github.com/yqm-307/bbtools-coroutine/issues/342)
- 本轮 Linux 生命周期修复：[#341](https://github.com/yqm-307/bbtools-coroutine/pull/341)
- 验证报告归档规则：[#330](https://github.com/yqm-307/bbtools-coroutine/issues/330)

## 2. 已确认的当前状态

PR #341 已合入 `main`，合并提交为 [`0c0e54ad`](https://github.com/yqm-307/bbtools-coroutine/commit/0c0e54ad36fb3c2462d611cd7eeede0c517a0ddc)。该 PR 收口了 Linux 侧等待登记、取消预检、Stop/重启代际和 parked 协程回收等生命周期问题，并增加了对应回归覆盖。

PR #341 的公开 CI 记录：

- 编译与单元测试：`SUCCESS`。
- 性能回归检查：`SUCCESS`。
- Sanitizer 检查：`SKIPPED`，不计为 Sanitizer 通过。
- CI 运行记录：[`34817101500`](https://github.com/yqm-307/bbtools-coroutine/actions/runs/34817101500)。

合并前的 Linux 本地记录还包括 Stop 竞态连续回归、排除已知基线超时后的 CTest，以及定向 sanitizer 运行。它们用于支撑 PR #341 的范围判断；原始日志、构建树和工具会话不作为仓库长期资产。

当前不能据此宣称以下内容已完成：

- Windows/MSVC 真实构建和运行；
- iOS 设备与模拟器构建和运行；
- 跨 worker 恢复和用户 TLS 边界的确定性验收；
- 代表性真实第三方 adapter 端到端验收；
- Issue #339 的完整三平台交付。

## 3. 契约与责任边界

核心运行时语义以[核心运行时契约](2026-09-07-core-runtime-contract.md)为唯一真源，验证与发布流程以[开发与发布流程](development-and-release-process.md)为准。本方案只记录跨平台实施范围和验收状态，不复制契约正文。

### 运行时负责

- 等待注册、结果发布、等待取消和完成/超时/取消竞争；
- 至多一次唤醒及运行时对象生命周期；
- 本库内部事件的驱动域、清理顺序和晚到通知安全；
- 明确不支持的句柄或平台能力，不用成功 stub 伪装支持。

### 第三方库与 adapter 负责

- 网络操作及其线程模型；
- 操作取消、socket、请求对象和业务缓冲区的生命周期；
- 在操作完成或取消确认前保活仍在途资源；
- 解除回调绑定、处理晚到通知，并保证通知不访问已销毁的协程对象；
- 按第三方库要求安排发起、取消和关闭的线程关系。

取消协程等待不等于第三方 I/O 已取消或完成。`Scheduler::Stop()` 不承诺自动取消任意第三方 I/O，也不代为关闭第三方 socket 或释放业务资源。

## 4. 目标事件适配形态

- 优先复用现有 FD、Timer 和 custom event 等等待能力。
- 平台句柄能力显式声明；Windows `SOCKET` 不能按 POSIX `int fd` 处理，Apple 平台句柄也不能未经验证套用 Linux 规则。
- 第三方回调先发布结果并通知运行时，不直接恢复用户协程；具体内存可见性、挂起前完成竞争和回调解绑由对应 adapter 验收。
- adapter 区分等待结果和第三方操作结果，保留第三方错误码和已传输字节数，不用“超时”覆盖部分传输。
- 事件等待登记不转移第三方 socket 所有权。借用、复制、关闭和注销规则必须在所选 adapter 中明确。

## 5. 状态矩阵

| 能力或阶段 | 当前状态 | 证据与说明 |
|---|---|---|
| 范围决策与责任边界 | 已确认 | [Issue #339](https://github.com/yqm-307/bbtools-coroutine/issues/339) 的公开范围决策 |
| Linux 等待生命周期修复 | 已合入 | [PR #341](https://github.com/yqm-307/bbtools-coroutine/pull/341)，合并提交 `0c0e54ad` |
| Linux 事件等待、取消和 Stop 局部回归 | 部分覆盖 | PR #341 的本地与 CI 记录；不等于完整 A4 |
| Linux 事件回调异常边界 | 部分覆盖 | PR #341 有相关实现和回归；真实第三方回调仍未验收 |
| 跨 worker 恢复 | 未验收 | 缺少针对同一协程迁移和事件完成后恢复的确定性证据 |
| 用户 TLS 边界 | 未验收 | 不能仅凭运行时内部 TLS 查询推断用户 TLS 安全 |
| Linux Hook | 平台专属，待矩阵化 | 保留 Linux 能力；不能作为 Windows/iOS 等价证据 |
| Windows x64/MSVC | 未验收 | 缺少真实 MSVC 构建、运行和消费示例证据 |
| iOS 设备 | 未验收 | 缺少 arm64 设备、签名和生命周期证据 |
| iOS 模拟器 | 未验收 | 缺少 Apple Silicon 模拟器独立运行证据 |
| 真实第三方 adapter | 未验收 | 尚未固定库、版本、adapter 入口和端到端矩阵 |
| 文档与结论级报告 | 进行中 | 由 [Issue #342](https://github.com/yqm-307/bbtools-coroutine/issues/342) 整理 |

状态含义：`已合入` 只表示代码进入 `main`；`已验证` 必须有对应真实证据；`部分覆盖` 不得扩写为完整通过；`未验收` 不表示失败，也不表示支持。

## 6. 后续阶段与门禁

### A4：运行时事件适配安全闭环

继续补齐驱动域、先完成后 park、重复/晚到通知、等待取消与 Stop、异常边界、跨 worker 和用户 TLS 验收。Linux 生命周期修复已合入，不代表 A4 全部关闭。

### A5：真实第三方 adapter

在实施前固定真实库、版本、adapter 入口和能力矩阵。至少覆盖正常交互、并发、超时、断连或 EOF、取消、Stop、晚到回调和资源生命周期。socketpair 或 mock 只能作为补充，不能替代真实服务交互。

### A6：公共平台基础

整理线程/锁、栈、句柄、静态/动态库和依赖声明，隔离平台专属编译参数。不得引入未确认的第三方依赖，不得通过删除模块或成功 stub 制造跨平台结果。

### A7：Windows/MSVC

分别记录构建、安装消费示例、核心等待能力、Stop 和真实 adapter 结果。MinGW 编译不能替代 MSVC 运行；Winsock 生命周期必须按实际使用路径核验。

### A8：iOS

分别记录设备和模拟器构建与运行。最小 App、前后台生命周期、线程、取消和真实 adapter 证据必须独立保存；macOS 通过不等于 iOS 通过。

### A9：三平台交付

只有共同 API、平台专属能力、Linux Hook 边界、依赖/许可证、支持矩阵和真实 adapter 证据齐全，且没有未处理 Critical/Important 问题，才可讨论整体完成。环境缺失时标记 `BLOCKED`，不以文档替代实测。

## 7. 证据和脱敏规则

- 长期规范、决策和方案放在 `agent-docs/`。
- 支撑重要结论的摘要放在 `tests/reports/archive/<kind>/<YYYYMMDD>-<sha>/`，只保留 `summary.md`、`summary.json` 和已有图表。
- 原始日志、完整时间序列、构建树、临时探针、工具会话和缓存不入仓。
- 公开文档只保留公开仓库 URL、Issue/PR 编号、公开 commit SHA、聚合测试结果和公开 CI 链接。
- 不写入凭据、认证信息、内部主机或网络信息、机器/账号信息、绝对本地路径、会话或进程标识。
- `SUCCESS`、`SKIPPED`、超时、部分覆盖和环境缺失必须保持原状态；不能互相替换。

## 8. 总体完成定义

Issue #339 的整体完成不是“Linux 单测通过”或“PR 已合入”。必须同时具备：

1. Linux、Windows、iOS 设备及承诺模拟器的共同核心能力证据；
2. 目标平台事件等待能力和 Linux Hook 边界的明确矩阵；
3. 至少一个代表性真实第三方 adapter 的端到端证据；
4. 运行时与 adapter 的资源责任、取消、Stop 和晚到通知边界一致；
5. 核心契约、用户文档、依赖说明、报告和实际实现一致；
6. 证据可由公开 Issue、PR、commit 和 CI 链接追溯；缺失环境明确记录为未验收或阻塞。

在上述条件满足前，Issue #339 保持开放。
