# M1-02a Coroutine 稳定 C++ API：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#268

## 已锁住

- 稳定方法：`Create` / `Resume` / `Yield` / `GetId` / `GetStatus`
- `ICoroutine` 含 Id、Status；可通过接口 Resume/Yield
- 类型仍在 `detail`，宏入口留给 #270

## 缺口

- `RequestCancel`：#266
- `GetException`：#267
- 不把 Coroutine 搬出 `detail`（避免一次公开 ABI 迁移）
