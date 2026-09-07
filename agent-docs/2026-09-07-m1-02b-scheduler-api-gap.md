# M1-02b Scheduler 稳定 C++ API：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#269

## 已锁住

- 稳定入口：`GetInstance` / `Start` / `Stop` / `LoopOnce` / `RegistCoroutineTask` / `IsRunning`
- `g_scheduler` 即单例
- `Start(THREAD)` 后 `LoopOnce` 仍 Assert
- `Start` 后再 `Start`（先 Stop）可冷重启

## 缺口

- Stop 唤醒全部等待留给 #280
- 不做多实例
- `OnActiveCoroutine` 仍是内部接口
