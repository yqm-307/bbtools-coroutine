#pragma once
#include <bbt/coroutine/syntax/Async.hpp>
#include <bbt/coroutine/syntax/_CoHelper.hpp>
#include <bbt/coroutine/syntax/_DeferHelper.hpp>
#include <bbt/coroutine/syntax/_EventHelper.hpp>
#include <bbt/coroutine/syntax/_WaitForHelper.hpp>

/**
 * @file SyntaxMacro.hpp
 * @brief 协程语法糖 API 手册：写协程代码时先读本文件，不必去读 Scheduler / Coroutine 的实现。
 *
 * 宏只做稳定 C++ API 的薄封装，不另起状态机；语义与它转调的底层接口一致。
 * 同一个意图有对应语法糖时用语法糖，不直接写底层调用；语法糖表达不了所需语义时，
 * 先补语法糖（见文末“已知缺口”），不要在业务代码里绕到内部实现。
 *
 * 速查（“协程内”指必须在协程上下文里调用，否则断言失败）：
 *
 * @verbatim
 *   意图                 用法                                   转调
 *   启动协程             bbtco [=]() {...};  bbtco_ref {...};   Scheduler::RegistCoroutineTask
 *   启动并取注册结果     bbtco_noexcept(&ok) [=]() {...};       RegistCoroutineTask(cb, bool&)
 *   启动并带诊断描述     bbtco_desc("name") [=]() {...};        RegistCoroutineTask(cb, desc)
 *   休眠（协程内）       bbtco_sleep(ms)                        Hook_Sleep
 *   让出执行权（协程内） bbtco_yield;                           Coroutine::YieldAndPushGCoQueue
 *   作用域结束时执行     bbtco_defer {...};                     detail::Defer（RAII 对象）
 *   等 fd 事件（协程内） bbtco_wait_for(fd, event, ms);         Coroutine::YieldUntilFdEx
 *   事件触发后起协程     bbtco_ev_r(fd) [=](int fd, short ev){} _EventHelper + CoPoller::CreateEvent
 *   创建同步原语         bbtco_make_comutex() / _cocond / _corwmutex / _copool(n)
 * @endverbatim
 *
 * 共同约定：
 *  - 被启动的协程默认栈为 GlobalConfig::m_cfg_stack_size（当前默认 12 KiB）；
 *    协程里要放大对象、深递归或调用栈开销大的库，必须显式指定栈大小，见“已知缺口”。
 *  - 协程可能在任意 worker 线程执行、挂起后可能换线程恢复：不要把线程局部指针、
 *    线程内锁（std::mutex 持有状态）带过挂起点。
 *  - lambda 捕获遵循普通 C++ 规则：协程异步运行，引用捕获 [&] 的对象必须活到协程结束；
 *    不能保证时用值捕获或 shared_ptr，不要借用栈上局部变量。
 *  - 不引入协程内抛出异常的恢复机制：协程体内的异常处理见契约文档。
 *
 * 已知缺口（尚无语法糖，直接用底层接口时在调用处注明原因）：
 *  - 指定栈大小启动协程：Scheduler::RegistCoroutineTask(stack_size, cb[, desc|succ])。
 *  - 指定目标 Processer 启动：Scheduler::RegistCoroutineTaskOn(target, [stack_size,] cb[, desc])。
 *  补语法糖时：宏只做薄封装、保持上表“意图 → 用法”的形式，并同步更新本手册和对应测试。
 */

/**
 * @name 辅助宏
 * @{
 */

/**
 * @brief 启动一个协程：`bbtco [=]() { ... };`
 *
 * 展开为 `_CoHelper()- lambda`，转调 `Scheduler::RegistCoroutineTask(cb)`，栈大小取全局默认。
 * 返回后协程只是被注册、不保证已开始执行；它会在任意时刻、任意 worker 线程执行。
 *
 * 所有权/生命期：lambda 被拷贝进协程，协程运行到结束才释放；引用捕获的对象必须活过协程。
 * 失败：注册失败（无 worker、栈申请失败等）抛出异常，调用点不会留下半注册的协程；
 *      需要不抛异常、自己处理失败时用 bbtco_noexcept。
 * 可在协程内、worker 线程或任意已初始化运行时之后的原生线程调用。
 */
#define bbtco bbt::coroutine::_CoHelper()-

/**
 * @brief 启动一个协程并默认引用捕获：`bbtco_ref { ... };`
 *
 * 等价于 `bbtco [&]() { ... }`，其余与 bbtco 相同。引用捕获意味着被引用的对象必须活到
 * 协程结束；协程比当前栈帧活得久时（常见情形）不要用它，改用 `bbtco [=]` 或 shared_ptr。
 */
#define bbtco_ref bbtco [&]()

/**
 * @brief 启动一个协程并取回注册结果：`bool ok; bbtco_noexcept(&ok) [=]() { ... };`
 *
 * 参数必须是 `bool*`（指向调用者持有的变量，不能为空）。展开为 `_CoHelper(succ)+ lambda`，
 * 转调 `RegistCoroutineTask(cb, bool&)`（noexcept）：注册成功写 true，任何异常都被吞掉并写 false。
 * 注册失败时协程没有被创建，lambda 不会执行；调用者负责处理失败（重试、释放已持有资源等）。
 */
#define bbtco_noexcept(succ) bbt::coroutine::_CoHelper(succ)+

/**
 * @brief 启动一个带描述的协程：`bbtco_desc("name") [=]() { ... };`
 *
 * desc 传给 `RegistCoroutineTask(cb, desc)` 并落入协程，worker 卡顿告警、诊断快照会读回它。
 * desc 必须是在协程整个生命期内有效的字符串（字面量最安全）；其余语义与 bbtco 一致。
 */
#define bbtco_desc(desc) bbt::coroutine::_CoHelper(desc)-

/**
 * @brief 协程内休眠：`bbtco_sleep(ms)`
 *
 * 只挂起当前协程，不阻塞 worker 线程，到时被唤醒（可能换到另一个 worker）。
 * 必须在协程内调用（否则断言失败）；ms <= 0 不休眠、直接返回 -1。返回值是挂起结果，
 * 不是剩余时间。
 */
#define bbtco_sleep(ms) bbt::coroutine::detail::Hook_Sleep(ms)

#define __bbtco_concat_impl(x, y) x##y
#define __bbtco_concat(x, y) __bbtco_concat_impl(x, y)
#define __bbtco_joint_suffix(name, suffix) __bbtco_concat(name, suffix)

#define __bbtco_defer_ex \
    bbt::coroutine::detail::Defer __bbtco_joint_suffix(bbtco_defer_, __COUNTER__) = bbt::coroutine::_DeferHelper()-

/**
 * @brief 作用域结束时执行：`bbtco_defer { ... };`
 *
 * 展开为一个局部 `detail::Defer` 对象，lambda 默认引用捕获 `[&]`；在对象所在作用域结束
 * （包括 return、异常展开）时执行，顺序与 C++ 析构一致：后声明的先执行。
 * 这是作用域级而不是“协程结束时”：写在函数里就是函数返回时执行。
 * 已有类型的析构能表达的清理仍用 RAII 对象，不必为了用宏再包一层 lambda。
 */
#define bbtco_defer \
    __bbtco_defer_ex [&]()

/**
 * @brief 让出执行权：`bbtco_yield;`
 *
 * 当前协程立即回到就绪队列，其他就绪协程先运行，之后可能在任意 worker 线程恢复。
 * 不在协程内调用时直接跳过（不报错）。
 * 展开自带 `do { ... } while(0);` 并已含结尾分号：不要写在没有花括号的
 * `if (...) bbtco_yield; else ...` 里（多出的分号会让 else 编译失败），写成 `if (...) { bbtco_yield; }`。
 */
#define bbtco_yield \
do \
{ \
    if (g_bbt_tls_coroutine_co == nullptr) \
        break; \
    g_bbt_tls_coroutine_co->YieldAndPushGCoQueue(); \
} while(0);

/**
 * sync：创建同步原语，均返回 shared_ptr；协程内使用，等待只挂起协程不阻塞线程
 */
#define bbtco_make_cocond()     bbt::coroutine::sync::CoCond::Create()
#define bbtco_make_comutex()    bbt::coroutine::sync::CoMutex::Create()
#define bbtco_make_corwmutex()  bbt::coroutine::sync::CoRWMutex::Create()

/**
 * copool：创建协程池，pool_max_co 为池内最大协程数；配合 bbtco_ev_*_with_copool 使用
 */
#define bbtco_make_copool(pool_max_co) bbt::coroutine::pool::CoPool::Create(pool_max_co)

/**
 * 禁止直接使用
 */
#define __bbtco_event_regist_ex(fd, event, time) (*std::make_shared<bbt::coroutine::_EventHelper>(fd, event, time))-
#define __bbtco_event_regist_with_copool_ex(fd, event, time, copool) (*std::make_shared<bbt::coroutine::_EventHelper>(fd, event, time, copool))-

#define bbtco_emev_timeout bbt::pollevent::EventOpt::TIMEOUT
#define bbtco_emev_readable bbt::pollevent::EventOpt::READABLE
#define bbtco_emev_writeable bbt::pollevent::EventOpt::WRITEABLE
#define bbtco_emev_close    bbt::pollevent::EventOpt::CLOSE
#define bbtco_emev_finalize bbt::pollevent::EventOpt::FINALIZE
#define bbtco_emev_persist  bbt::pollevent::EventOpt::PERSIST

/**
 * @brief 注册一次性 fd/定时器事件，触发后新建协程（或交给协程池）执行回调：
 *        `bbtco_ev_r(fd) [=](int fd, short event) { ... };`
 *
 * 函数格式：bbtco_ev_[event type]、bbtco_ev_[event type]_with_copool；
 * with_copool 版本把回调提交给指定协程池，池销毁后回退到直接启动协程。
 *
 * 事件字母：[w] 可写  [r] 可读  [c] 关闭（FINALIZE）  [t] 超时（带 timeout_ms 参数）；
 * bbtco_ev_t(timeout_ms) 是纯定时器（fd = -1）。
 *
 * 语义：
 *  - 一次性：触发一次、回调执行完就释放事件；不支持 PERSIST（传入即断言失败）。
 *  - 回调签名固定 `void(int fd, short trigger_event)`，在新协程中运行，可以挂起。
 *  - 返回 int：事件类型非法时返回 -1；否则为 Event::StartListen 的返回值（-1 表示登记失败）。
 *    登记失败时回调不会被调用。
 *  - 生命期：事件对象与回调由内部持有到触发后释放；fd 的所有权和关闭仍归调用者，
 *    回调里使用 fd 前自行确认它没被关闭。
 */
#define bbtco_ev_w(fd)  __bbtco_event_regist_ex(fd, bbtco_emev_writeable, -1)
#define bbtco_ev_r(fd)  __bbtco_event_regist_ex(fd, bbtco_emev_readable, -1)
#define bbtco_ev_wc(fd) __bbtco_event_regist_ex(fd, bbtco_emev_writeable | bbtco_emev_finalize, -1)
#define bbtco_ev_rc(fd) __bbtco_event_regist_ex(fd, bbtco_emev_readable | bbtco_emev_finalize, -1)
#define bbtco_ev_wt(fd, timeout_ms) __bbtco_event_regist_ex(fd, bbtco_emev_writeable | bbtco_emev_timeout, timeout_ms)
#define bbtco_ev_rt(fd, timeout_ms) __bbtco_event_regist_ex(fd, bbtco_emev_readable | bbtco_emev_timeout, timeout_ms)
#define bbtco_ev_wtc(fd, timeout_ms) __bbtco_event_regist_ex(fd, bbtco_emev_writeable | bbtco_emev_timeout | bbtco_emev_finalize, timeout_ms)
#define bbtco_ev_rtc(fd, timeout_ms) __bbtco_event_regist_ex(fd, bbtco_emev_readable | bbtco_emev_timeout | bbtco_emev_finalize, timeout_ms)
#define bbtco_ev_t(timeout_ms) __bbtco_event_regist_ex(-1, bbtco_emev_timeout, timeout_ms)

#define bbtco_ev_w_with_copool(fd, copool)  __bbtco_event_regist_with_copool_ex(fd, bbtco_emev_writeable, -1, copool)
#define bbtco_ev_r_with_copool(fd, copool)  __bbtco_event_regist_with_copool_ex(fd, bbtco_emev_readable, -1, copool)
#define bbtco_ev_wc_with_copool(fd, copool) __bbtco_event_regist_with_copool_ex(fd, bbtco_emev_writeable | bbtco_emev_finalize, -1, copool)
#define bbtco_ev_rc_with_copool(fd, copool) __bbtco_event_regist_with_copool_ex(fd, bbtco_emev_readable | bbtco_emev_finalize, -1, copool)
#define bbtco_ev_wt_with_copool(fd, timeout_ms, copool) __bbtco_event_regist_with_copool_ex(fd, bbtco_emev_writeable | bbtco_emev_timeout, timeout_ms, copool)
#define bbtco_ev_rt_with_copool(fd, timeout_ms, copool) __bbtco_event_regist_with_copool_ex(fd, bbtco_emev_readable | bbtco_emev_timeout, timeout_ms, copool)
#define bbtco_ev_wtc_with_copool(fd, timeout_ms, copool) __bbtco_event_regist_with_copool_ex(fd, bbtco_emev_writeable | bbtco_emev_timeout | bbtco_emev_finalize, timeout_ms, copool)
#define bbtco_ev_rtc_with_copool(fd, timeout_ms, copool) __bbtco_event_regist_with_copool_ex(fd, bbtco_emev_readable | bbtco_emev_timeout | bbtco_emev_finalize, timeout_ms, copool)
#define bbtco_ev_t_with_copool(timeout_ms, copool) __bbtco_event_regist_with_copool_ex(-1, bbtco_emev_timeout, timeout_ms, copool)

/**
 * @brief 等待 fd 事件：`bbtco_wait_for(fd, bbtco_emev_readable, 100);`
 *
 * 当前协程挂起，直到 fd 发生指定事件或超时，不阻塞 worker 线程。
 * 必须在协程内调用（否则断言失败）；等待失败时抛 std::runtime_error("wait for failed!")，
 * 所以超时/失败要用 try/catch 或在外层统一处理，不会返回错误码。
 * 展开为一个以 `__COUNTER__` 命名的局部变量声明，等待发生在构造函数里：
 * 它是语句，不能出现在表达式中，同一作用域可以连续写多次。
 *
 * @param fd 文件描述符
 * @param event 事件类型，参考 bbtco_emev_*
 * @param ms 超时时间，单位毫秒
 */
#define bbtco_wait_for(fd, event, ms) \
    bbt::coroutine::_WaitForHelper __bbtco_joint_suffix(_WaitForHelper_, __COUNTER__){fd, event, ms}

/**
 * @}
 */
