# M1-01d 异常与 Stop：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#267

## 已锁住

- 运行时边界捕获异常，`GetException()` 交付 `exception_ptr`
- 无回调：计数；有回调：不计数
- 回调自身抛出被隔离，不逃出 fcontext
- `YieldUntilTimeout` 超时是返回值，不抛异常
- `Scheduler::Stop()` 不排空业务等待（5s sleep 不会拖住 Stop）

## 缺口（留给 #280 / 工具层）

- Stop 不唤醒所有等待事件；靠 Processer 退出
- Timeout/Closed/Rejected 仍是各工具返回码，核心不新增枚举
- 无 Task/Join；detached 无接收者只有计数/回调
