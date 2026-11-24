# 坑点与禁止行为

当前行为。写调用代码时按此检查。

## 生命周期

- `bbtco` detached。`[&]` 捕获的栈对象必须活到该协程结束，否则改堆或 `shared_ptr`。
- 无业务停机入口（`Stop` 已删除）：运行时随进程寿命存活。要「全部跑完」：自己用 latch/Chan 收齐后再返回 main；等待中的协程随进程结束，不做栈展开。
- 未初始化（未 `Start`）时 `bbtco` 抛 `std::runtime_error("scheduler has no processer: coroutine task rejected")`。`bbtco_noexcept(&succ)` 置 `succ=false`，不抛。
- 重复 / 并发 `Start()` 抛 `std::logic_error`；不再重置配置或队列。

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

`bbtco_sleep` 只能在受管协程中调用；原生线程使用普通睡眠 API。见 [睡眠说明](../../../../docs/guides/user-guide.md#33-bbtco_sleepms)。

## 同步原语

- `CoMutex::Lock()` 返回 `void`。禁止 `auto x = mu->Lock()`。用 `CoLockGuard` / `CoUniqueLock`。
- 同协程对同一 `CoMutex` 重入：底层 assert。
- `CoRWMutex` 不允许持读锁再拿写锁（assert）。有写者排队时新读者会等。
- `CoSelect` 不支持 `Chan<T,0>`，编译期 `static_assert`。
- `CoSelect` 不持有传入的 Chan；命中下标也不等于读写成功。见 [CoSelect API](../../../../docs/guides/api-reference.md#5-coselect)。
- `CoCond` 不会替你释放业务锁，也不保存提前发出的通知。任务完成用 Chan；见 [CoCond API](../../../../docs/guides/api-reference.md#7-cocond)。
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

## async / bind / await / fire

- 顺序 async 函数用 `bbt_co_async(T)` 声明返回类型，在函数体内用 `bbt_co_await`，并用 `bbt_co_return(value)` 或 `bbt_co_return_void` 返回。函数体从调用开始执行。
- `bbt_co_bind` 返回惰性 `AsyncOperation<T>`，裸调用只构造操作，不执行绑定的操作体。
- 当前协程需要最终结果、错误或完成顺序时用 `bbt_co_await`。
- 明确放弃最终结果的惰性 detached 副作用才用 `bbt_co_fire`。顺序 async 函数体不能依赖 `fire` 推迟执行，有顺序依赖时不能用 `fire`。
- 普通函数或短 lambda 用 `bbt_co_bind` 构造操作，不要手写 `detail::MakeAsyncOperation`。长操作绑定命名函数，不把整段逻辑塞进 lambda。
- 保存的操作要用 `std::move` 消费，操作只能消费一次。
- 不要让操作体借用调用方的栈变量、引用或 `std::string_view` 指向的临时存储。
- `bbt_co_await` 和 `bbt_co_fire` 只接受 async 操作。普通值不能伪装成可等待操作。
