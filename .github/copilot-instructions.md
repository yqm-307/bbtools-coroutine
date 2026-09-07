# bbtools-coroutine · Copilot 指南
- 仅支持 Linux 的 Go 风格协程运行时，基于 boost.context 实现，并与 bbtools-core 的日志、计时、线程工具和事件循环深度集成。

## 语言约定
- 除非用户明确要求使用英文或其他语言，否则所有说明、分析、总结、提问与最终回复默认使用简体中文。
- 代码、命令、配置键、协议字段、API 名称、库名、文件名及其他技术标识保持原文，不做强制翻译。
- 当输出同时包含自然语言与代码时，自然语言部分使用简体中文，代码保持原始语法与命名。
- 当需要生成、建议或实际执行 `git commit` 时，提交信息默认使用简体中文；仅在用户明确要求其他语言或仓库已有明确提交规范冲突时例外。

## 架构
- 核心契约真源是 `agent-docs/2026-09-07-core-runtime-contract.md`；涉及核心行为先读它，本文不重复完整契约。
- 项目需求、决策、进度和验收的仓库消息真源是对应 GitHub Issue/PR 及其评论；读取、授权回写和冲突处理遵循 `AGENTS.md` 的“仓库消息真源”规则。
- 核心实现位于 `bbt/coroutine/detail/...`。当前 `Scheduler` 协调 `Processer`、`CoPoller`、`Coroutine`、`StackPool` 和 `Profiler`；M1 目标是让 `Scheduler` 通过 `EventLoop` 与具体 `Poller` 解耦。
- `CoPoller`、协程事件和底层事件循环正在收敛为 `EventLoop / Poller / CoroutineEvent` 职责边界；不要把当前 epoll/libevent 关系当成冻结 API。
- `Hook.{cc,hpp}` 重载 socket/connect/read/write 等调用；`LocalThread` 的 TLS 控制 Hook 是否在当前线程生效。Hook 目标是保持原生语义，不能为扩大覆盖而静默改变 flags、timeout、errno 或阻塞行为。
- `GlobalConfig`（`detail/GlobalConfig.hpp`）须在 `Start()` 前设置好栈大小、调度线程数、扫描间隔及栈池上限等参数。
- `StackPool.{cc,hpp}` 复用带 mprotect 的协程栈，并按 `GlobalConfig` 的阈值周期性收缩。

## 代码模式
- 引入 `bbt/coroutine/coroutine.hpp` 后要调用 `g_scheduler->Start()/Stop()`；`Scheduler::Start` 默认使用 `SCHE_START_OPT_SCHE_THREAD`，也支持 loop/no-loop 模式。
- 推荐用 `bbt/coroutine/syntax` 下的语法糖创建协程：`bbtco [](){...};`、`bbtco_desc("name")`、`bbtco_ref`、`bbtco_noexcept(successFlag)`。`bbtco_desc` 的诊断描述持久化属于 M1 目标，当前实现是否保存描述须以代码和测试为准。
- `bbtco_defer` 展开为 `detail::Defer`，提供 Go 式延迟清理，优先于手写 try/finally。
- 协程里用 `bbtco_sleep(ms)`（底层 Hook_Sleep）替代 `std::this_thread::sleep_for`，这样才能通过 Poller 正确让出执行权。
- 事件辅助宏（`bbtco_wait_for`、`bbtco_ev_r/w/t[_with_copool]`）会注册 `CoPollEvent`，可选地把回调派发到指定 `CoPool` 中执行。
- `g_bbt_tls_helper->EnableUseCo()` 控制 Hook 是否生效；若手动创建新线程要跑协程，记得先调用 `LocalThread::SetEnableUseCo(true)`。

## 并发原语
- `Chan`、`CoCond`、`CoMutex`、`CoRWMutex`、`CoPool` 属于可迭代工具层，不定义核心状态机；具体行为以各自 API、测试和迁移说明为准。
- 工具层应复用核心通用等待、唤醒、超时、取消和关闭机制，不反向依赖具体 Poller 实现。
- `pool::CoPool` 创建固定数量的 worker 协程；`Release` 的停止/取消/排空语义必须以实现文档为准，不能默认理解为“所有已提交任务完成”。
- 长时间阻塞 I/O 是否能安全协程化，遵循 `agent-docs/2026-09-07-core-runtime-contract.md` 的 Hook 契约和兼容矩阵，不用“放在协程外处理”掩盖未验证边界。

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
