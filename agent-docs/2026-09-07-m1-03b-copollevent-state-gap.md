# M1-03b CoroutineEvent 等待状态机：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#272

## 已锁住

- 一次等待：`Init*` / `Regist` / `CommitPark` / `Trigger` / `UnRegist`
- 重复 Trigger 只有第一次有效；FINAL / CANCELLED 不可复用
- 超时与自定义唤醒竞态：先到者胜
- 对端关闭 fd：走底层 READABLE，完成一次
- 析构：`DeferDestroyEvent`；weak 回调过期不触发
- `GetStatus()` 是 `CoPollEventPhase` 的粗映射

## 缺口

- `IPollEvent` 未接入，本阶段不实现
- 关闭不是独立阶段（没有 `CLOSED` phase）
- 不换 Poller backend（#273）
