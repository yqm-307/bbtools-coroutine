# M1-03d EventLoop 契约测试与依赖检查：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#274

## 已锁住

- FD / Timer / Wakeup 各有契约测试，走 `CoPoller::PollOnce` / `NotifyCustomEvent`
- Scheduler 循环只调 `g_bbt_poller->PollOnce()`，换 backend 改 `CoPoller`
- `IPoller` 仍是未接入遗留接口

## 缺口

- `Scheduler.hpp` 经 `Define.hpp` 泄漏 `sys/epoll.h`：#271
- ASIO 泄漏守卫：#273
- 不实现可替换 `IPoller`，不做第二次 backend
