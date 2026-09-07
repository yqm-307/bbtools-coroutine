# M1-03a EventLoop / Poller 接口边界：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#271

## 已锁住

- `CoPoller` 是 EventLoop 门面：`PollOnce` / `CreateEvent` / `NotifyCustomEvent`
- Scheduler 只调 `PollOnce`，头文件不再漏 `sys/epoll.h`
- 三类事件：FD / Timer / Wakeup（`NotifyCustomEvent`）
- epoll 不是用户契约；backend 是 `bbt::pollevent::EventLoop`

## 缺口

- `IPoller` 仍未实现，不在本阶段接入
- 不替换 backend（#273）
- CoroutineEvent 状态机留给 #272
