# M1-02c 宏薄封装：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#270

## 已锁住

- `bbtco` / `bbtco_desc` / `bbtco_noexcept` 都走 `RegistCoroutineTask`
- `bbtco_yield` 走 `YieldAndPushGCoQueue`
- `bbtco_sleep` 走 `Hook_Sleep`
- 宏不另起状态机

## 缺口

- `bbtco_desc` 丢掉 desc，描述落库留给 #276
- README 仍写“带描述的协程”，留给 #250 / #282
