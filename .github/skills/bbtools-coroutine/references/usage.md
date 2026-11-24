# 调用方式索引

写调用代码先找 [同场景的可运行示例](../../../../examples/README.md)，再读 [使用指南](../../../../docs/guides/user-guide.md) 核对边界。

按任务继续阅读：

- 完整签名、返回码和前置条件，读 [`docs/guides/api-reference.md`](../../../../docs/guides/api-reference.md)。
- `bbt_co_async`、`bbt_co_bind`、`bbt_co_await` 和 `bbt_co_fire`，读 [`docs/guides/async-usage.md`](../../../../docs/guides/async-usage.md)。
- 运行时、进程寿命、worker 迁移和网络执行域，读 [`docs/architecture/core-runtime-contract.md`](../../../../docs/architecture/core-runtime-contract.md)。
- 宏的展开边界和语法细节，读 `bbt/coroutine/syntax/SyntaxMacro.hpp`。
- 常见禁止行为，读本目录的 [`pitfalls.md`](pitfalls.md)。

## Agent 编码规则

- 入口优先使用 `BBTCO_MAIN*`，常规应用不自行调用 `g_scheduler->Start()`。
- `bbtco` 是 detached 注册，注册顺序、开始时间和 worker 都不保证。引用捕获的对象必须活到协程结束。
- `Chan`、`CoCond`、等待型 `CoMutex` / `CoRWMutex`、`bbtco_wait_for` 和 `CoSelect::Run` 必须在受管协程中调用。
- 协程挂起后可能迁移 worker，不要把线程局部状态、worker 指针或 native 线程锁带过挂起点。
- 主任务返回前收口业务资源。runtime 没有业务 `Stop()`，也不会替应用等待 detached 协程。
- 顺序 async 流程使用 `bbt_co_async(T)`、`bbt_co_await` 和 `bbt_co_return`。需要保存 callable 与参数并延迟执行时使用 `bbt_co_bind`。
- `bbt_co_fire` 只用于明确放弃最终结果的惰性 detached 操作。顺序 async 函数体不能依赖 `fire` 推迟执行。
- 不要用 native `future::wait/get`、`condition_variable` 或 `join` 代替协程等待。
- 任务包含长时间 CPU 计算时主动拆分并 `bbtco_yield`，或使用合适的执行池。
