# bbtools-coroutine · Copilot 指南
- 仅支持 Linux 的 Go 风格协程运行时，基于 boost.context 实现，并与 bbtools-core 的日志、计时、线程工具和事件循环深度集成。

## 架构
- 核心实现位于 `bbt/coroutine/detail/...`，`Scheduler` 负责协调各 `Processer` 线程、`CoPoller`、`Coroutine`、`StackPool` 以及 `Profiler`。
- `Scheduler` 为每个优先级维护一组 `moodycamel::BlockingConcurrentQueue`，并通过 `Processer::Steal` 做任务窃取以平衡负载。
- `CoPoller` 包装了 `bbt::pollevent::EventLoop`（基于 libevent），让 `Coroutine::YieldUntil*` 能挂起等待 fd、超时或自定义事件。
- `Hook.{cc,hpp}` 重载 socket/connect/read/write 等阻塞调用；`LocalThread` 提供的 TLS 确保 Hook 只在调度线程内生效。
- `GlobalConfig`（`detail/GlobalConfig.hpp`）须在 `Start()` 前设置好栈大小、调度线程数、扫描间隔及栈池上限等参数。
- `StackPool.{cc,hpp}` 复用带 mprotect 的协程栈，并按 `GlobalConfig` 的阈值周期性收缩。

## 代码模式
- 引入 `bbt/coroutine/coroutine.hpp` 后要调用 `g_scheduler->Start()/Stop()`；`Scheduler::Start` 默认使用 `SCHE_START_OPT_SCHE_THREAD`，也支持 loop/no-loop 模式。
- 推荐用 `bbt/coroutine/syntax` 下的语法糖创建协程：`bbtco [](){...};`、`bbtco_desc("name")`、`bbtco_ref`、`bbtco_noexcept(successFlag)`。
- `bbtco_defer` 展开为 `detail::Defer`，提供 Go 式延迟清理，优先于手写 try/finally。
- 协程里用 `bbtco_sleep(ms)`（底层 Hook_Sleep）替代 `std::this_thread::sleep_for`，这样才能通过 Poller 正确让出执行权。
- 事件辅助宏（`bbtco_wait_for`、`bbtco_ev_r/w/t[_with_copool]`）会注册 `CoPollEvent`，可选地把回调派发到指定 `CoPool` 中执行。
- `g_bbt_tls_helper->EnableUseCo()` 控制 Hook 是否生效；若手动创建新线程要跑协程，记得先调用 `LocalThread::SetEnableUseCo(true)`。

## 并发原语
- `sync::Chan<T,N>` 通过 `CoWaiter` + `Coroutine::Yield*` 实现缓冲/无缓冲通道，并提供支持超时的 `TryRead/TryWrite`。
- `sync::CoCond`、`CoMutex`、`CoRWMutex` 依赖自定义 poll 事件实现等待/唤醒，只能在调度线程中使用。
- `pool::CoPool` 创建固定数量的 worker 协程；`Submit` 把任务丢进 `moodycamel::ConcurrentQueue`，`Release` 会阻塞到所有任务完成。
- `CoPool` 借助原子计数统计启用了 defer 的任务，避免过早退出；长时间阻塞 I/O 应放在协程外处理。

## 日常流程
- 快速构建：执行 `./build.sh`（Release + examples），默认依赖 bbtools-core、boost_context、libevent 并 `sudo` 安装头文件和动态库到 `/usr/local`。
- 手动构建：`cmake -B build -DNEED_EXAMPLE=ON -DNEED_TEST=ON .. && cmake --build build`，调试产物位于 `build/bin/*`。
- 仅在编译时开启 `-DNEED_TEST=ON` 后再运行单测；进入 `build/` 执行 `ctest --output-on-failure`。
- 示例程序在 `example/`，开启 `NEED_EXAMPLE` 后编译并从 `build/bin/example/` 运行。
- 基准测试在 `benchmark_test/`，需 `-DNEED_BENCHMARK=ON`，可配合 `benchmark_test/run.sh`。

## 集成提示
- 链接时需要 `libbbt_coroutine.so` 和 bbtools-core；安装脚本会把头文件放到 `/usr/local/include/bbt/coroutine/`。
- `context/fcontext.hpp` 是自带的 Boost.Context fcontext，实现 ABI 稳定；扩展栈时与 `GlobalConfig` 的栈大小保持一致。
- `utils/lockfree` 内嵌 moodycamel 队列，为头文件实现；使用时避免在队列回调里阻塞。
- 调试开关：`-DDEBUG_INFO` 或 `-DSTRINGENT_DEBUG` 打开详细日志，`-DPROFILE` 启用 `detail::Profiler` 采样。
- 若需扩展新的阻塞 syscall，请在 `Hook.cc` 中按 `YieldUntilFdReadable/Writeable` 模式处理 errno，保持与现有 Hook 行为一致。
