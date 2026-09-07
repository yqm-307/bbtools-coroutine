# M1-04a Hook blocking FD：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#260

## 已锁住

- 库不期望调用方传入 blocking fd；协程 IO 路径统一由 Hook 兜底。
- 所有会挂起的 Hook IO 入口（Connect/Read/Write/Accept/Send/Recv/SendTo/RecvFrom/RecvMsg/SendMsg/Readv/Writev/Accept4）先经 `CoIoNonblockGuard`：IO 期间临时 `O_NONBLOCK`，返回时还原调用方原 flags。
- `fcntl` 设不了非阻塞则直接失败返回（-1），禁止在 blocking fd 上执行会卡 worker 的 syscall。
- `RecvMsg/SendMsg` 保留 `MSG_DONTWAIT` 直通原语义，不加守卫。
- 回归测试 `Test_hook_blocking_fd`：外部 blocking socketpair 上 read 挂起期间其他协程可继续跑（ticker 证明 worker 未被占死），read 返回后 fd flags 恢复为无 `O_NONBLOCK`。

## 已知限制（ponytail 注释在案）

- 按次设置/还原：多协程并发共享同一 blocking fd 时存在 flags 竞态窗口；当前契约下单 fd 同一时刻只允许一个协程 IO，暂不处理。
- Hook 生效前提仍是 LD_PRELOAD/syscall 拦截路径，与 #263（第三方客户端兼容）无关，不在本阶段扩大。

## 验证

- `Test_hook_blocking_fd` 1/1 通过（修复前同测试 timeout 8 复现卡死，RED 成立）。
- `Test_hook_contract` 29/29 通过，无回归。
