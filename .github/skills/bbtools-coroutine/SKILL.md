---
name: bbtools-coroutine
description: >
  Use when writing, reviewing, or debugging application code that calls
  bbtools-coroutine (bbtco, Scheduler, Chan, CoMutex, CoPool, Hook).
  Load before inventing signatures or copying README tables.
---

# bbtools-coroutine

写**调用本库**的代码时用本 skill。改运行时实现改读 `AGENTS.md` 和 `agent-docs/2026-09-07-core-runtime-contract.md`。

签名冲突：头文件 + 测试 > `agent-docs/api-reference.md` > README 速查表。不要凭速查表补重载。

## 1. 架构简介

有栈协程，boost.context 切换，仅 Linux。全局单例 `g_scheduler` 管调度；多个 Processer（worker 线程）跑协程。同一协程同一时刻只在一个 worker 上；挂起后可在别的 worker 恢复。

`EventLoop` / `CoPoller` 驱动 fd、定时器、唤醒。协程里的阻塞 syscall 由 Hook 转成等待，不占死 worker；非协程上下文直通原生。

用户同步工具（`Chan`、`CoMutex`、`CoCond`、`CoPool`、宏）不是调度器状态机。协程是 detached：`bbtco` 注册后不 join。

详细：`references/architecture.md`

## 2. 用法

| 章节 | 何时读 |
|------|--------|
| 基本用法 | `Start` / `bbtco` / `sleep` / `Stop` |
| 进阶用法 | Chan、锁、CoSelect、事件、Hook IO、配置 |
| 使用范式 | 生产消费、锁+条件变量、CoPool、事件驱动 |

正文：`references/usage.md`

## 3. API 文档

只引用，不在 skill 里复制签名：

- `agent-docs/api-reference.md` — 当前用户 API（签名、返回码、挂起、前置条件）
- 对应 `.hpp` — 签名真源

未出现在 API 参考里的符号：打开头文件，不要猜。

## 4. 坑点与禁止行为

必读，写代码前过一遍。展开：`references/pitfalls.md`

禁止：

- 在即将销毁的栈变量上挂协程引用捕获（`bbtco` 是 detached）
- 把 `Stop()` 当成「等任务跑完」
- `Start(SCHE_START_OPT_SCHE_THREAD)` 之后调用 `LoopOnce()`
- 非协程上下文调用会挂起的 API（`Wait` / `Lock` 等待 / `Chan` 阻塞读写 / `bbtco_wait_for`）
- `CoSelect` 搭配 `Chan<T,0>`
- 为「避免阻塞」手动改 FD 的 `O_NONBLOCK`；多线程对同一 fd 并发 IO
- 在 `CoPool` 任务里跑长时间 CPU 循环占住池协程
- 发明新宏或让宏承载另一套状态机
- 用 `auto x = mutex->Lock()`（`Lock()` 返回 `void`）
- 停机后再 `bbtco` 还不接异常 / 不查 `bbtco_noexcept` 的 `succ`
