# M1-01a 协程状态机：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#264

本文记录当前实现与 M1 目标契约的差异。实现已用 `Test_coroutine` 锁住；下列缺口不在 #264 改，避免把 #266/#267 提前做完。

## 已锁住的实现

```
Create → CO_RUNNABLE
Resume → CO_RUNNING
Yield / YieldWithCallback / YieldAndPushGCoQueue → CO_SUSPEND
Resume ← CO_SUSPEND（事件入队不改写状态）
返回或异常 → CO_FINAL
```

`CO_DEFAULT` 只是成员初值，`Create()` 返回后不可观察。`Yield` 之后不再回到 `CO_RUNNABLE`。

非法 `Resume(CO_RUNNING|CO_FINAL)`、非法 `Yield(!CO_RUNNING)` 走 `Assert`，不是可观察错误码。

## 相对契约的缺口

| 契约 | 当前 | 后续 |
|---|---|---|
| 协作式取消 | 无 Cancel API，无独立状态 | #266 |
| 失败与完成可区分；`exception_ptr` 交付 | 失败与完成同为 `CO_FINAL`，只计数/回调 | #267 |
| 最小现场含描述、等待对象 | `GetStatus`/`GetId` 可查，无描述 | #276 |
