# 坑点与禁止行为

当前行为。写调用代码时按此检查。

## 生命周期

- `bbtco` detached。`[&]` 捕获的栈对象必须活到该协程结束，否则改堆或 `shared_ptr`。
- `Stop()` 取消式停机：不再接新任务；等待中的协程被唤醒并销毁；队列里未执行的回收。**不保证**正在跑或尚未跑的业务完成。要「全部跑完」：自己用 latch/Chan 收齐，再 `Stop`。
- `Stop` 之后 `bbtco` 抛 `std::runtime_error("scheduler stopped: coroutine task rejected")`。`bbtco_noexcept(&succ)` 置 `succ=false`，不抛。

## 调度

- 默认 `Start()` 已有后台调度线程，禁止再 `LoopOnce()`（assert）。手动驱动必须 `Start(SCHE_START_OPT_SCHE_NO_LOOP)`。
- 注册后的协程不保证顺序。
- 用户态死循环或未 Hook 的阻塞调用会占住 worker。可设 `m_cfg_worker_stall_warn_ms` 观察。

## 必须在协程内

下列调用在非协程上下文会失败或没有协程语义：

- `bbtco_wait_for`（assert：`must be in coroutine context`）
- `CoCond::Wait` / `WaitFor`
- `CoMutex::Lock` 及会等待的 `TryLock(ms)`
- `Chan` 的阻塞 `Read`/`Write`
- `CoSelect::Run`

`bbtco_sleep` 在非协程走 Hook 的原生睡眠分支；应在协程内使用。

## 同步原语

- `CoMutex::Lock()` 返回 `void`。禁止 `auto x = mu->Lock()`。用 `CoLockGuard` / `CoUniqueLock`。
- 同协程对同一 `CoMutex` 重入：底层 assert。
- `CoRWMutex` 不允许持读锁再拿写锁（assert）。有写者排队时新读者会等。
- `CoSelect` 不支持 `Chan<T,0>`，编译期 `static_assert`。
- `CoSelect` 的 Chan 与输出变量必须活到 `Run()` 返回。
- `CoCond` 析构时若仍有等待者，会打调试日志；先唤醒或保证等待者已退出。
- `<<` / `>>` 丢失 `Write`/`Read` 的 `-1`/`-2` 细节。需要区分关闭与错误时用返回码。

## CoPool

- 池大小固定，不能动态加 worker。
- 任务挂起会占用那条池协程，后续任务排队。
- `Release()` 丢掉队列中未执行的任务；`SubmitWithFuture` 可能 `broken_promise`。`get()` 前检查 `valid()` 并接 `future_error`。
- `Submit` 在池已 `Release` 时返回 `-1`。

## Hook / FD

- 不要为「协程里不能阻塞」去 `fcntl` 改 flags。协程 IO 期间库会临时设 `O_NONBLOCK`，返回时恢复。
- 不要多线程对同一 fd 并发做 Hook 路径上的 IO（flags 会被改回）。
- `MSG_DONTWAIT` 保持立即返回。
- 等待中的 fd 被 `close`：等待协程被唤醒，syscall 返回 `EBADF`。

## 异常与栈

- detached 协程里抛出的异常不会进主线程。无回调时记日志并增加 `m_unhandled_exception_count`，不 `terminate`。生产应设 `m_ext_coevent_exception_callback`。回调自己再抛异常会被隔离。
- 默认栈 `m_cfg_stack_size = 12 * 1024`。大对象放堆上。
- 默认开启栈保护页，溢出是 `SIGSEGV`。不要关 `m_cfg_stack_protect` 除非接受未定义行为。

## 宏

宏只映射已有 C++ API。禁止新增一套只存在于宏里的状态机。`bbtco_ref { }` 是引用捕获块，不是 `[]()`。
