# M1-04b Hook flags 与 socket timeout 语义：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#261

## 已锁住

- `MSG_DONTWAIT` 在 send/recv/sendto/recvfrom 与 msg 家族统一直通原生函数：无数据立即 -1/EAGAIN，不进协程等待。
- `SO_RCVTIMEO`/`SO_SNDTIMEO` 在协程 IO 路径生效为**有界协程等待**：`IoTimeout` 在 IO 入口快照 socket 选项并计算 deadline；#260 守卫把 fd 临时 O_NONBLOCK 后内核 timeout 不再触发，由 Hook 用剩余时间挂起，到期重试仍 EAGAIN 则返回 -1 且 errno=EAGAIN（`ErrnoGuard` 保持）。对齐 POSIX「超时到期 -1/EAGAIN（EWOULDBLOCK）」。
- 覆盖面按 Linux socket(7) 口径：内核 timeout 适用于所有 socket IO 系统调用（read/write 走 sock_recvmsg/sock_sendmsg，readv/writev 走 iter 路径，accept 受 SO_RCVTIMEO 约束），故 IoTimeout 应用于 recv 家族、send 家族、read/write/readv/writev、accept/accept4 全部挂起路径（独立审查发现的缺口，非 POSIX 误读）。
- 未设置 timeout 或非 socket（getsockopt 失败，如常规文件/管道）走无限协程等待，行为与 #260 后一致。
- 等待期间 worker 不占死（ticker 协程推进验证）。

## 已知限制（ponytail 注释在案）

- 超时 1ms 粒度，亚毫秒向上取整；POSIX 允许 timeout 用掉即返回，不承诺与内核逐微秒一致。
- `Hook_Connect` 未接 IoTimeout：connect 超时的 POSIX 返回是 ETIMEDOUT 且需区分「连接完成 vs 超时」并重置 socket 状态，与 IO 的 EAGAIN 语义不同；不在 #261 范围，如需有界 connect 另立 issue。
- 数据与超时同刻竞态时优先重试 syscall，可能返回数据而非 EAGAIN（与内核竞态语义一致）。
- tv_sec 超过 ~24.8 天会 int 溢出退回无限等待（内核上限 ~25 天，实际不可达）。

## 验证

- `Test_hook_timeout_flags` 7/7：MSG_DONTWAIT recv/sendto 立即 EAGAIN；SO_RCVTIMEO/SO_SNDTIMEO 有界返回（≥40ms、<5s）；未到期数据到达正常返回；read() 亦受 SO_RCVTIMEO 约束；常规文件读取不受影响。修复前该测试 timeout 20 全挂（RED 复现无限等待）。
- 回归：`Test_hook_contract` 29/29、`Test_hook_blocking_fd` 1/1、`Test_poller` 4/4、`Test_copollevent_state` 14/14。
