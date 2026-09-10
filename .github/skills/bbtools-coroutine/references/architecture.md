# 架构简介

只描述当前运行结构，不讲演进。

## 组件

```
应用代码
  bbtco / 同步原语 / Hook 拦截的 syscall
        │
        ▼
   Scheduler（单例 g_scheduler）
        │  注册、全局队列、停机
        ▼
   Processer × N（worker 线程）
        │  Resume 当前协程
        ▼
   Coroutine（有栈，boost.context）
        │  挂起时
        ▼
   CoPoller / EventLoop
        fd / timer / wakeup → 再入队 Resume
```

- **Scheduler**：创建 worker、把任务丢给 Processer 或全局队列、work steal、`Start`/`Stop`。
- **Processer**：每线程一个。从本地队列或全局队列取协程执行。
- **Coroutine**：用户函数跑在独立栈上。`Yield` 后栈冻结，唤醒后从断点继续，可能换 worker。
- **CoPoller**：等待 fd/定时器就绪，唤醒对应协程。底层 Poller 可替换；用户代码不依赖 epoll。
- **Hook**：动态链接把 POSIX 阻塞调用替换为协程等待。只在「当前线程正在跑协程」时生效。

## 执行模型

- 多 worker 并行。
- 单个协程同一时刻只在一个 worker 上。
- 挂起后允许恢复到其他 worker。
- TLS（`g_bbt_tls_processer` / `g_bbt_tls_coroutine_co`）只在 worker 线程、当前协程栈上有效。
- 无抢占：用户死循环会占住该 worker。
- 取消是协作式：`RequestCancel` 置标志，代码在检查点或等待返回后自行退出，走 C++ 析构。

## 生命周期

1. `g_scheduler->Start()` 拉起 worker（默认后台线程）。
2. `bbtco` → `RegistCoroutineTask` → 某个 Processer 执行。
3. 协程函数返回则销毁；`Stop()` 停止接新任务、唤醒并回收等待中的协程、join worker。不保证业务任务执行完。

## 工具层

`Chan`、`CoMutex`、`CoRWMutex`、`CoCond`、`CoPool`、`bbtco_*` 宏：挂起时走同一套等待/唤醒，但可以独立迭代。宏必须能映射到已有 C++ API。
