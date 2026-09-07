# M1-04c Hook POSIX 错误、EOF、关闭与降级矩阵

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#262

## 调用矩阵（协程内；非协程一律直通原生）

状态：支持 = Hook 挂起/唤醒并保持语义；直通 = 原生 syscall 语义原样。

| 调用 | 场景 | 原生语义 | Hook 语义 | 状态 | 证据 |
|---|---|---|---|---|---|
| read/recv/readv/recvmsg | EOF（对端关闭） | 0 | 0 | 支持 | matrix t_eof + contract |
| write/send/writev | 对端已关 | -1/EPIPE(+SIGPIPE) | -1/EPIPE | 支持 | matrix t_epipe |
| recv | 对端 RST | -1/ECONNRESET | -1/ECONNRESET | 支持 | matrix t_econnreset |
| 全部 IO | fd 无效 | -1/EBADF | -1/EBADF | 支持 | contract 各 EBADF 用例 |
| sleep/poll/read 等 | EINTR | 重启/返回 | 循环内重试，errno 保持 | 支持 | contract eintr 用例 |
| read/recv 等 | 等待中 fd 被 close | （阻塞线程被 EBADF 唤醒） | 唤醒等待协程，重试得 -1/EBADF | 支持（本次修复） | matrix t_close_during_wait |
| poll/select | 等待中 fd 被 close | epoll 静默移除 | 同上：WakeupFdWaiters 唤醒 | 支持（同一机制） | PollCore 复用 CoPollEvent |
| accept/connect | fd 关闭 | -1/EBADF | 同上 | 支持（同一机制） | — |
| send/recv/sendto/recvfrom | MSG_DONTWAIT | 立即 -1/EAGAIN | 直通原生 | 支持 | #261 + timeout_flags |
| socket fd 的 SO_*TIMEO | 有界等待 | -1/EAGAIN | 有界协程等待 -1/EAGAIN | 支持 | #261 |
| 常规文件/管道 | SO_*TIMEO 不存在 | 无 timeout | getsockopt 失败→无限等待 | 支持 | timeout_flags t_regular_file |
| DNS getaddrinfo | Scheduler::Stop 在途 | — | 3s 内返回不挂死 | 支持 | contract stop 用例 |

## 明确限制 / 降级

- 唤醒机制：Linux 下 close 被 epoll 关注的 fd 会**静默移除关注项**（不产生事件），
  原生线程靠 close 把阻塞 syscall 以 EBADF 弹回；协程挂起在事件上则收不到，
  会永久挂起。修复：`CoPollEvent` 维护 fd→等待事件注册表，`Hook_Close` 在原生
  close 成功后调 `WakeupFdWaiters`，被唤醒方重试 syscall 得 -1/EBADF。
- 竞态窗口（接受）：注册表在 Regist 进入 ARMED 后插入；close 与 Regist 交错时，
  最迟由唤醒方重试路径 + 下一轮 close 收敛。单 fd 同一时刻单协程 IO 为契约前提。
- 非 Hook_Close 通道的 fd 关闭（直接 syscall 绕过 hook 拦截）不在本机制覆盖内。
- ECONNRESET 探针用 TCP loopback + SO_LINGER(1,0) RST；AF_UNIX 无 RST 语义。
- Hook_Connect 超时语义（ETIMEDOUT）不在本轮，见 04b 已知限制。

## 验证

- 新增 `Test_hook_error_matrix` 4/4（EOF/EPIPE/ECONNRESET/等待中 close 唤醒），
  其中 close 唤醒用例修复前 RED（3s 内协程不返回）。
- 回归全绿：`Test_hook_contract` 29/29、`Test_hook_blocking_fd`、`Test_hook_timeout_flags` 7/7、`Test_poller` 4/4、`Test_copollevent_state` 14/14。
