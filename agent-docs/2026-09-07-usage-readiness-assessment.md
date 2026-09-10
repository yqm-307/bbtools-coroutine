# bbtools-coroutine 使用期评估

**状态：** 2026-09-10 收口（#286）。已进入受控使用期。
**版本：** `v3.0.0-rc1`（`d2ab06e053e6dbf558865ee99f3cef5b02641b6c`）
**权威：** 使用边界以本文为准；核心语义仍以 [`2026-09-07-core-runtime-contract.md`](./2026-09-07-core-runtime-contract.md) 为准。

> 本文取代 2026-09-07 评估中已过时的探针结论（`MSG_DONTWAIT` 会挂起、Release 栈泄漏、`bbtco_desc` 空壳、`CoPool::Release` 不排空等）。那些行为已由后续 Issue 修掉并锁进测试。

## 结论

允许作为自用服务的试点底座，版本钉 `v3.0.0-rc1`。不承诺「任意阻塞式框架透明兼容」。

用户 2026-09-10 决定关闭 #256 与 #286。#286 原文依赖 #284 / #285；本次不把 Hook 客户端长 soak、异常/停机资源长跑当作进入受控使用期的前置。#259 / #284 / #285 保持开放。

## 可以使用

- 已知 Hook 覆盖的 Linux 网络、时间、多路复用和 DNS 路径；
- 应用遵守协程生命周期、FD 和停机约束；
- 服务有外部健康检查、请求超时和进程重启手段；
- 已对实际客户端做过断连、重连和停机验证的场景。

调用入口：[`user-guide.md`](./user-guide.md)、[`.github/skills/bbtools-coroutine/SKILL.md`](../.github/skills/bbtools-coroutine/SKILL.md)。坑点以 [`pitfalls.md`](../.github/skills/bbtools-coroutine/references/pitfalls.md) 为准。

## 暂不承诺

- 任意第三方框架或客户端的透明兼容；
- 未走 Hook 的阻塞 syscall 自动转协程等待；
- 用户态死循环自动恢复；
- 崩溃或栈溢出时给出完整协程现场；
- `Scheduler::Stop()` / `CoPool::Release()` 排空业务任务；
- `Hook_Connect` 有界超时（`ETIMEDOUT`，见 #261 已知限制）；
- 真实客户端「服务端慢响应注入」超时场景（#256 明确边界）。

## 2026-09-07 最低条件对照

| 条件 | 状态 | 证据 |
|---|---|---|
| Release 栈释放 | 已修 | #279，`t_stack_clear_releases_and_is_idempotent` |
| blocking FD / `MSG_DONTWAIT` / `SO_*TIMEO` | 已修 | #260 / #261 / #262；`Test_hook_blocking_fd`、`Test_hook_timeout_flags`、`Test_hook_error_matrix` |
| CoPool 生命周期 | 已修 | #281，`t_release_drains_pending_futures`（取消式排空，不保证执行） |
| 协程最小现场 | 已修 | #276，`Test_coroutine_diag` |
| worker 无进展检测 | 已修 | #277，`Test_worker_stall`（默认阈值 0=关闭） |
| 真实客户端试运行 | 已验 | #263 / #283：Echo + hiredis；CTest `Test_real_clients` |

停机契约（#280）和 detached 异常日志+计数（#275）一并入 main，见 `Test_scheduler_stop`、`Test_exception_stop`。

## #256 收口

子项 #260 / #261 / #262 / #263 已关。父项验收里「至少一个真实客户端完成超时测试」未做：服务端慢响应注入不在 `scripts/acceptance_real_clients.py` 覆盖面（见 [`2026-09-09-real-client-acceptance.md`](./2026-09-09-real-client-acceptance.md)）。Timeout 语义由 #261 单测锁住。用户决定将该项记为边界后关闭 #256。

## 发布证据

- Release：[v3.0.0-rc1](https://github.com/yqm-307/bbtools-coroutine/releases/tag/v3.0.0-rc1)（prerelease）
- Gate：[actions/runs/34385647909](https://github.com/yqm-307/bbtools-coroutine/actions/runs/34385647909)（`source_sha=d2ab06e`）
  - 全量 CTest：success
  - 真实客户端验收：success（Redis 缺失必须失败，本 run 未 skip）
  - 六模块并行疲劳：success，约 60 分钟（`18:01:48Z`–`19:01:51Z`）
  - 性能发布 Gate：success，带 WARN 注解（不阻断；Stable 前人工看）
- 评估日 `origin/main` 为 `2e9f330`（#324 文档，运行时与 RC 相同）

#263 本地补充：Release 构建，单轮 14s；20 轮 273s、fails=0。真实客户端超时注入仍未覆盖。

## 留给 #284 / #285

- #284：Hook 真实兼容长 soak（现有是 273s 本地重复跑 + RC 1h 六模块压测，不是客户端长 soak）
- #285：异常 / 取消 / Stop / 资源趋势在真实服务上的长跑（单测已有，长跑未单独取证）
- Redis 重启重连、完整协议 framing、全部第三方客户端

## 最终判断

骨架成立，2026-09-07 列出的底座门槛已封闭。进入受控试点，不全面推广。后续重点是真实负载下的长测与边界，而不是继续加 Hook 数量。
