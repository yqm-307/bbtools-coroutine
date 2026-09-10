---
name: bbtools-coroutine
description: >
  Use when writing, reviewing, or debugging application code that calls
  bbtools-coroutine (bbtco, Scheduler, Chan, CoMutex, CoPool, Hook).
  Load before inventing signatures or copy-pasting README tables.
---

# bbtools-coroutine 使用

写调用本库的代码时先读本 skill。改运行时实现仍以 `AGENTS.md` 和核心契约为准。

## 先读

1. `agent-docs/user-guide.md` — 最小程序、组合、禁区
2. `agent-docs/api-reference.md` — 按符号的签名、返回码、挂起语义
3. 对应头文件 — 签名真源；文档与头文件冲突时以头文件 + 测试为准

不要凭 README 速查表补全重载或返回码。README 只作入门。

## 硬规则

- Linux only。入口 `#include <bbt/coroutine/coroutine.hpp>`。
- 先 `g_scheduler->Start()`，最后 `Stop()`。`Start` 后不要再 `LoopOnce`。
- `bbtco_sleep` / `bbtco_yield` / `bbtco_wait_for` / 同步原语 Wait/Lock/Chan 读写：必须在协程内。
- 返回码优先：`0` 成功、`-1` 失败、`1` 超时。`Chan` 另有 `-2` 错误。
- `Stop()` 是取消式停机，不排空业务。需要“跑完”就自己 latch，再 `Stop`。
- `Stop` 后 `bbtco` 抛 `std::runtime_error`；`bbtco_noexcept(&succ)` 置 `succ=false`。
- `CoSelect` 不支持 `Chan<T,0>`（`static_assert`）。
- 不要在协程闭包里捕获即将销毁的栈引用，除非生命周期覆盖该协程。
- Hook 只在协程上下文把阻塞 syscall 转等待；非协程直通原生。不要为“避免阻塞”去改 FD flags。
- 宏只映射已有 C++ API，禁止发明新宏或新状态机。

## 常用入口

| 目的 | API |
|------|-----|
| 注册协程 | `bbtco` / `bbtco_desc("name")` / `bbtco_noexcept(&succ)` |
| 睡眠 / 让出 | `bbtco_sleep(ms)` / `bbtco_yield` |
| 通道 | `sync::Chan<T, N>`，`Write`/`Read` 或 `<<` / `>>` |
| 锁 | `bbtco_make_comutex()` + `CoLockGuard<CoMutex>` |
| 条件变量 | `bbtco_make_cocond()`，`Wait` / `NotifyOne` / `NotifyAll` |
| 池 | `bbtco_make_copool(n)`，结束用 `Release()`（取消式排空） |

完整签名与禁区见 `agent-docs/api-reference.md`。

## 完成前自检

- 示例能编译：对照 `example/` 或 `unit_test/`，不要手写未存在的 API。
- 时间单位是毫秒，除非头文件写明微秒（`GlobalConfig` 里部分字段是微秒）。
- 未在参考文档出现的符号：先打开头文件，标「待核实」，不要猜。
