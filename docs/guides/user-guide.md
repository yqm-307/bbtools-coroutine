# bbtools-coroutine 使用指南

先从 [可运行示例](../../examples/README.md) 找和任务最接近的程序，再来这里查怎么选 API、哪些条件不能省。签名和返回码查 [API 参考](api-reference.md)；运行时语义查 [核心运行时契约](../architecture/core-runtime-contract.md)。

## 1. 入口、依赖和最小程序

### 1.1 环境

Linux、C++17、Boost ≥ 1.90（`boost.context`）。包含 `<bbt/coroutine/coroutine.hpp>` 并链接 `bbt_coroutine`。构建命令见 [示例索引](../../examples/README.md)。

### 1.2 推荐的最小入口

从 [启动并等待一个协程](../../examples/coroutine/main.cc) 开始。常规应用定义 `int bbtco_main()`，由 `BBTCO_MAIN()` 生成原生 `main`。不要自己启动 scheduler 或创建 runtime runner 线程。

### 1.3 入口宏怎么选

| 需要 | 入口 |
| --- | --- |
| 默认配置 | `BBTCO_MAIN()` |
| 设定初始 worker 数、默认栈或栈预算 | `BBTCO_MAIN_WITH_OPTIONS(...)` |
| 启动前配置完整 `GlobalConfig` | `BBTCO_MAIN_WITH_SETUP()`，在 `setup_runtime()` 中配置 |
| 启动后、提交主任务前运行原生代码 | `BBTCO_MAIN_WITH_HOOK()`，如 [MySQL 启动探针](../../examples/mysqlpp/README.md) |
| 提交主任务后在原生主线程进入宿主阻塞入口 | `BBTCO_MAIN_WITH_SETUP_AND_HOST_HOOK()` |

组合入口和 hook 签名见 [RuntimeMacros.hpp](../../bbt/coroutine/syntax/RuntimeMacros.hpp)。`RuntimeOptions` 不是完整 `GlobalConfig` 的替代；启动后不要改全局启动配置。

### 1.4 主任务完成和进程退出

runtime 只等待 `bbtco_main()` 返回，不替应用排空 detached 协程，也没有业务 `Stop()`。用缓冲 `Chan` 收齐结果后再返回，不用固定时长的睡眠猜任务是否结束。池任务需要先收结果再 `Release()`；只释放池可能丢弃队列里的任务。

## 2. 执行模型

协程有自己的栈，多个 worker 可以并行执行不同协程。同一协程同一时刻只在一个 worker 上运行，但挂起后可换别的 worker 恢复。不要把线程 ID 当协程 ID，也不要把 TLS 引用、worker 指针或持有的 `std::mutex` 带过挂起点。

调度是协作式的，不抢占执行中的协程。会挂起的 API（如 `Chan::Read()`、`CoMutex::Lock()`、`CoCond::Wait()`）在受管协程内调用。在协程中不要用原生 latch、`future.wait/get`、`condition_variable` 或 `join()` 等待，否则会占住整个 worker。

## 3. 创建协程、让出和延时

### 3.1 注册 detached 协程

`bbtco` 注册后不提供 `join`，不保证开始时间、注册顺序或执行线程。`bbtco_desc("name")` 只是加诊断名称；用生命周期足够长的名字，例如字符串字面量。

优先按值捕获，需要共享寿命时捕获 `shared_ptr`。引用捕获（包括 `bbtco_ref`）不会延寿。注册失败默认抛异常；要显式检查失败，用 `bbtco_noexcept(&ok)` 并处理 `ok == false`，见 [Echo 服务端](../../examples/echoserver/server.cc) 关闭未能交给协程的连接。

### 3.2 `bbtco_yield`

让出当前协程并把它放回就绪队列，不是等待另一个任务完成。把 CPU 工作拆成小步时可以主动让出；不要假设下一步在哪个 worker 恢复。单行 `if` 使用本宏时加花括号。

### 3.3 `bbtco_sleep(ms)`

只在受管协程里使用；它挂起当前协程而不是 worker。`ms <= 0` 不睡眠。原生线程用 `std::this_thread::sleep_for`；被 Hook 的 POSIX `sleep` 在原生线程走原生分支，**不等于** `bbtco_sleep` 可以在协程外用。

### 3.4 查询执行上下文

`IsInCoroutineContext()` 查询当前上下文。`GetLocalCoroutineId()` 和 `GetLocalCoroutineStackSize()` 分别给出协程 ID 和可用栈字节数，协程外均为 `0`。只在确实要求协程上下文的业务入口用 `RequireCoroutineContext()` 做前置检查。

## 4. 等待和事件

### 4.1 等待文件描述符

- `bbtco_wait_for(fd, event, ms)` 等待后继续当前协程。正常返回不区分 fd 就绪和超时；登记失败或取消会抛异常。
- `bbtco_ev_*` 登记一次性事件，触发后另起协程执行回调；调用方保有 fd 并保证在回调使用前未关闭。见 [定时器示例](../../examples/waitfor/README.md) 对两种写法的对照。
- 需要区分就绪、超时、取消或自定义通知时，用 `CoWaiter` 的状态接口；见 [fd 和回调适配](../../examples/socketpair_adapter/README.md)。启动可能同步完成的外部操作时，先登记等待，再在 `on_registered` 中投递。

### 4.2 Hook I/O

[本地 TCP Echo](../../examples/echoserver/README.md) 使用 `accept`、`recv` 和 `send`，每个连接由一个协程持有，处理 TCP 短写和 EOF。不要为启用 Hook 而手动切换 fd 的 `O_NONBLOCK`；`MSG_DONTWAIT` 仍表示立即返回。不要在多个线程并发对同一个 fd 执行 Hook I/O。

Hook 只改变支持的系统调用如何等待，不保证第三方库线程安全，也不会替你处理第三方库自己的锁或线程局部状态。看 [hiredis 示例](../../examples/redis/README.md) 的独占连接，以及 [MySQL 示例](../../examples/mysqlpp/README.md) 把同步探针留在原生线程的做法。

## 5. 同步原语怎么选

| 场景 | 推荐能力 |
| --- | --- |
| 传递任务、结果或完成信号 | 缓冲 `Chan<T, N>`，需要当面交接时用 `Chan<T, 0>` |
| 等多个缓冲通道 | `CoSelect` |
| 保护共享状态 | `CoMutex` + `CoLockGuard` |
| 读多写少 | `CoRWMutex` + `CoReadLock` / `CoWriteLock` |
| 唤醒已登记的等待者，不保存通知 | `CoCond` |
| 固定并发数执行任务 | `CoPool` |

### 5.1 Chan 生产者消费者

[Chan 示例](../../examples/chan/main.cc) 用同一段代码演示有缓冲和无缓冲交接。元素按值传递；普通 `Read` / `Write` 成功返回 `0`。需要区分关闭与失败时，不要用只返回 `bool` 的 `>>` / `<<`。

生产者都不会再写以后才能关闭通道。缓冲 Chan 的 `Read()` 在关闭后仍可取完已入队的数据，读空后返回 `-1`。不要用 `TryRead()` 来做这个收尾：它遇到已经关闭的 Chan 会直接返回 `-1`。其他失败及带超时的返回码见 [Chan API](api-reference.md#4-chan)。

零缓冲 `Chan<T,0>` 是真当面交接：读写双方都就绪才完成一次交接，无隐藏存储。无等待读者时，无超时 `TryWrite` 返回 `-2` 且不存值（不同于旧版暂存一个元素），限时版超时返回 `1`；`Close()` 唤醒仍在等待的写者返回 `-1`。等待的读者、写者各按入队 FIFO 匹配，支持多生产者/多消费者。依赖旧「暂存一个元素」的代码须先建立读者或改用限时写，详见 [Chan API](api-reference.md#4-chan) 的迁移说明。

### 5.2 CoSelect 多路等待

[CoSelect 示例](../../examples/select/main.cc) 保持通道打开。命中下标不一定代表读写成功：关闭也算就绪，而且不会填充读出变量。`-1` 只代表没选中，也可能是竞争失败，不等同于超时。需要区分来源结束时，可以先用打开的通道传递显式结束消息，再由调用方收口。

引用和 `shared_ptr` 重载都不持有 Chan。调用方要保留 Chan 和读出变量，直到 `Run()` 返回；`CaseWrite` 按值保存待写值。只支持有缓冲的 Chan，见 [CoSelect API](api-reference.md#5-coselect)。

### 5.3 CoMutex

`Lock()` 返回 `void`，不是成功状态。优先用 RAII guard 自动解锁；同一协程不能重复获取同一把 `CoMutex`。需要立即尝试或限时等待时，用 `TryLock()` 或 `TryLock(ms)`，并检查返回值。见 [锁示例](../../examples/comutex/README.md)。

### 5.4 CoCond

`Wait()` / `WaitFor()` 不会替你释放业务锁。持锁等会挡住需要同一把锁的通知者；先解锁再等又可能错过通知。不要把它当成 `std::condition_variable::wait(lock, pred)`；任务完成和状态变化优先用能保存消息的缓冲 Chan。

[CoCond 示例](../../examples/cocond/README.md) 在一个 worker 上演示先登记、再通知；多 worker 时不能用注册顺序或固定延时保证这个先后。条件对象必须活过等待者，返回码见 [CoCond API](api-reference.md#7-cocond)。

### 5.5 CoRWMutex

有写者排队时，新读者等待。用 `CoReadLock` / `CoWriteLock` 缩小临界区；不能持有读锁时再申请写锁。见 [锁示例](../../examples/comutex/main.cc)。

## 6. 协程池和固定并发

### 6.1 默认协程池

[协程池示例](../../examples/copool/main.cc) 检查 `Submit()`，从缓冲 Chan 收齐结果，再在池外 `Release()`。结果通道容量等于提交数；即使主协程因提交失败提前退出，已开始的任务也不会卡在发送结果上。业务任务可能失败时，也要向接收方交付失败结果。

`Release()` 拒绝新任务、等待在途 worker 退出、丢弃未开始的队列任务。它不是「做完所有排队任务」；不能从本池 worker 内调用，否则抛 `std::logic_error`。

### 6.2 需要结果时使用 `SubmitWithFuture`

该 API 返回 `std::future<void>`，交付完成或异常，不带业务值。先检查 `valid()`；`get()` 可能阻塞，只在原生线程使用。协程内要等结果用 Chan。池异常的回调和计数见 [CoPool API](api-reference.md#8-copool)。

### 6.3 长任务和池容量

池协程挂起在 I/O 上仍占一个池名额；CPU 重任务还会占住执行它的 worker。尺寸路由池的栈规格、并发和队列容量在创建时固定；`Submit(StackRequest, ...)` 必须精确匹配已声明的 `(usable_bytes, protect)`，不借用其他规格的 worker。见 [尺寸路由示例](../../examples/copool/main.cc)。

## 7. async 操作

[async 示例](../../examples/async/main.cc) 演示两步顺序等待、提前返回、只移动结果和 detached 投递。

- 顺序 `bbt_co_async(T)` 从调用时执行，在受管协程中用 `bbt_co_await` 调用；`bbt_co_return value;` 包装最终结果，不改变执行时机。
- `bbt_co_bind` 按值保存 callable 和参数，消费时才执行。保存后的操作用 `std::move` 交给 `bbt_co_await` 或 `bbt_co_fire`，只能消费一次。
- `bbt_co_fire` 只用于惰性操作，且不返回最终结果。需要等待它的业务副作用时，应另设完成通知；不能靠 `fire` 推迟顺序函数体。

复制 `std::string_view`、裸指针或 `std::ref` 不会延长被借用对象的寿命。类型、引用返回和异常细节见 [Async 调用指南](async-usage.md)。

## 8. 资源清理、异常和取消

### 8.1 `bbtco_defer`

在所在作用域退出时执行，包括提前返回和异常展开；后声明的先执行。已有 RAII 类型时优先用类型自身析构。宏默认引用捕获，不要引用更早失效的对象。见 [fd 关闭示例](../../examples/echoserver/client.cc)。

### 8.2 异常

detached 协程的异常不会回到提交者的调用栈。要把业务失败交给调用方，用返回码、结果类型或 Chan。运行时和 CoPool 的异常回调要短小，不要在回调里阻塞 worker。

### 8.3 协作式取消

`RequestCancel()` 置位并尝试唤醒当前等待。它不会强杀协程，也不保证能打断任意原生阻塞调用。业务代码在检查点或等待返回后自行退出，正常走析构；能区分取消与超时的接口见 [API 参考](api-reference.md#取消)。

## 9. 栈、性能和线程边界

默认协程栈为 12 KiB，创建后不扩栈。大缓冲优先放在堆上；第三方库或深调用链需要测栈预算，必要时用 `RuntimeOptions::default_stack` 或尺寸路由池。专用 [栈预算工具](stack-budget.md) 的入口不能当作应用模板。

把长 CPU 工作拆成小步骤主动让出，或交给合适的执行池。遇到 worker 卡顿可先看 `m_cfg_worker_stall_warn_ms` 诊断，再按 [性能指南](performance.md) 测量，不凭示例的一次耗时判断性能。
