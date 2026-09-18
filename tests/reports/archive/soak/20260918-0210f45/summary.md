# M1-07b/c 本地 soak 摘要（#284 Hook 真实兼容长测，#285 异常取消停机与资源验收）

## 结论

- **#284 修了 1 个真 bug 再验证通过**：高并发加断连组合下，同一 fd 的旧 asio 监听仍在延迟销毁队列里时，新等待抢先注册同 fd，epoll ADD 报 `EEXIST`，异常逃出杀死连接协程，客户端 5s 超时。修复：`CoPollEvent::InitFdEvent` 有界重试（3 次×1ms，直达内核睡眠避开 nanosleep Hook 重入），仍失败返回 -1 不抛异常；新增回归单测修前红修后绿。修复后同条件 **60/60 轮全过**，零异常零断言。
- **#285 常驻验证通过**：常驻 echo 服务约 4 分钟 30 波零失败；RSS 6748KB→6840KB（+1.4%，阶梯状，疑似 `s_waiters` 惰性表，需更长运行确认）；`terminate` 正常退出；kill 后重启可接受新连接并回显；异常/取消/停机相关 8 个单测全绿。285 范围内的可观测性被 284 的 bug 反向自证：异常有日志加协程 id、有计数、无静默丢失、无崩溃。
- **hiredis 未跑**：本地无 redis-server 与 hiredis 头文件（且无 sudo 安装），第三方 FD 长测记为未覆盖，不伪造通过。

## 环境

- worktree commit：`0210f45`（`hermes/hermes-880173d8`，基于 `origin/main`）
- 构建：Release，`NEED_TEST=ON NEED_EXAMPLE=ON`，bbt-core 头文件与 `libbbt_core.so` 复用主仓 `build-asio-validation/core-wt-a2-issue8` 产物；运行时 `LD_LIBRARY_PATH` 指向该目录与本次构建 `lib`
- 入口：`scripts/acceptance_real_clients.py` 的 echo 部分（12 客户端×3 重连×16 消息加 8 异常断连，seed 固定）；常驻波次脚本见本 PR 描述

## #284 验证数据

- 修前：10 轮 1 败，12 轮 2 败；服务端日志均为 `[bbtco] unhandled exception co=N: assign: File exists [system:17 ... reactive_descriptor_service.ipp:120 in function 'assign']`，客户端 `TimeoutError('timed out')`
- 最小化：去断连 10 轮全过，单客户端加断连 10 轮全过，只有“高并发加断连”组合复现
- 修后：同条件 60/60 全过；失败现场日志类关键字（unhandled/Assertion/give up）零命中
- 单测：`Test_copollevent_state`（含新增 `t_init_fd_event_with_live_duplicate_never_throws`）及 hook/异常/停机/诊断 8 套件全绿；新增用例在修复摘除后确认红（抛异常且返回值非 -1）
- 修中插曲：第一版重试用 `std::this_thread::sleep_for`，经堆栈证实走了 nanosleep Hook 造成协程内重入 double-wait；已改直达内核 `syscall(SYS_nanosleep)`，库内 Hook 路径睡眠须走原始 syscall（与 `EpGuard` 同一戒律）

## #285 验证数据

- 常驻 30 波（每波 4 客户端×8 消息，间隔 8s，共 245s）：失败波 0；服务端日志坏行 0
- RSS（KB）：6748 起，6840 止，max 6840；序列 `6748,6756,6760,6764,6772,6780,6784,6784,6784,6788,6792,6796,6800,6808,6808,6812,6812,6812,6812,6816,6820,6820,6820,6820,6820,6824,6828,6828,6832,6840`
- 停机：`terminate` 5s 内退出（rc=-15）；重启后新连接回显 PASS
- 相关单测：`Test_exception_stop`、`Test_scheduler_stop`、`Test_worker_stall`、`Test_coroutine_diag` 全绿

## 未覆盖或仍未完成

- hiredis 第三方 FD 长测：本地缺 redis-server 与 hiredis，需 CI 发布 Gate（`release.yml` 真实客户端验收）覆盖
- 服务端慢响应注入超时：#256 已记为明确边界，本次未覆盖
- RSS +92KB（+1.4%）趋势：4 分钟太短，只能说无突增，是否惰性表稳态需小时级运行确认
- 完整 CTest 未跑：只跑了与本次相关的 8 个套件；全量与 sanitizer 交给 PR CI
- #284 / #285 / #259 保持开放，由维护者勾选关闭
