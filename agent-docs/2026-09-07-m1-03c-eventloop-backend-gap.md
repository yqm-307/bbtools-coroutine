# M1-03c 现有 backend 接入 EventLoop：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#273（标题写 epoll，实现已是 ASIO）

## 已锁住

- `CoPoller` 已接入 `bbt::pollevent::EventLoop`
- `PollOnce()` 调 `StartLoop(LOOP_NONBLOCK)`，不直接 `epoll_wait`
- `Scheduler.hpp` 不泄漏 ASIO
- 首个 backend 是 ASIO EventLoop，epoll 不是用户契约

## 缺口

- Issue 标题「epoll backend」过时；当前 backend 是 ASIO
- `IPoller` 仍未实现，不在本阶段接入
- Hook 的 `poll()` 聚合仍用 `epoll_create1`，属 Hook 层，不搬进 CoPoller
- 不替换 backend、不多实例
