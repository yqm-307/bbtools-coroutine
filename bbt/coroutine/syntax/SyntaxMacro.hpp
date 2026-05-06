#pragma once
#include <bbt/coroutine/syntax/_CoHelper.hpp>
#include <bbt/coroutine/syntax/_DeferHelper.hpp>
#include <bbt/coroutine/syntax/_EventHelper.hpp>
#include <bbt/coroutine/syntax/_WaitForHelper.hpp>

/* 辅助宏 */

/**
 * @brief 注册一个协程任务
 *
 * 前置条件：scheduler 已启动。
 * 失败模式：当 scheduler 未运行时，会沿用 RegistCoroutineTask 的
 * std::runtime_error 失败路径。
 * 调度语义：注册成功后，任务会在某个调度 worker 上异步执行，
 * 不保证执行顺序，也不保证固定线程。
 */
#define bbtco bbt::coroutine::_CoHelper()-

/**
 * @brief 注册一个协程任务，默认使用引用捕获，其他契约与 bbtco 相同
 */
#define bbtco_ref bbtco [&]()

/**
 * @brief 注册一个协程任务，并在注册时记录是否成功
 *
 * 不抛出异常；注册失败时会把 succ 置为 false。
 */
#define bbtco_noexcept(succ) bbt::coroutine::_CoHelper(succ)+ 

/**
 * @brief 注册一个协程任务，实际上和 bbtco 一致，只是增强可读性
 */
#define bbtco_desc(desc) bbt::coroutine::_CoHelper(nullptr, (desc))-

/**
 * @brief 在协程中调用，使协程挂起一定时间后被唤醒
 *
 * 前置条件：当前必须处于协程上下文。
 * 失败模式：非协程上下文会触发断言；ms <= 0 时返回失败码。
 */
#define bbtco_sleep(ms) bbt::coroutine::detail::Hook_Sleep(ms)

#define __bbtco_concat_impl(x, y) x##y
#define __bbtco_concat(x, y) __bbtco_concat_impl(x, y)
#define __bbtco_joint_suffix(name, suffix) __bbtco_concat(name, suffix)

#define __bbtco_defer_ex \
    bbt::coroutine::detail::Defer __bbtco_joint_suffix(bbtco_defer_, __COUNTER__) = bbt::coroutine::_DeferHelper()-

/**
 * @brief defer语法糖，defer的操作会在协程执行完毕后调用。和c++析构顺序一致
 */
#define bbtco_defer \
    __bbtco_defer_ex [&]()

/**
 * @brief 将当前协程挂起。协程立即进入 runnable 路径，后续由 scheduler 重新调度
 *
 * 在非协程上下文中是 no-op，不会隐式创建协程或抛异常。
 */
#define bbtco_yield \
do \
{ \
    if (g_bbt_tls_coroutine_co == nullptr) \
        break; \
    g_bbt_tls_coroutine_co->YieldAndPushGCoQueue(); \
} while(0);

/* sync */
#define bbtco_make_cocond()     bbt::coroutine::sync::CoCond::Create()
#define bbtco_make_comutex()    bbt::coroutine::sync::CoMutex::Create()
#define bbtco_make_corwmutex()  bbt::coroutine::sync::CoRWMutex::Create()
/* copool */
#define bbtco_make_copool(pool_max_co) bbt::coroutine::pool::CoPool::Create(pool_max_co)

/* 禁止直接使用 */
#define __bbtco_event_regist_ex(fd, event, time) (*std::make_shared<bbt::coroutine::_EventHelper>(fd, event, time))-
#define __bbtco_event_regist_with_copool_ex(fd, event, time, copool) (*std::make_shared<bbt::coroutine::_EventHelper>(fd, event, time, copool))-

#define bbtco_emev_timeout bbt::pollevent::EventOpt::TIMEOUT
#define bbtco_emev_readable bbt::pollevent::EventOpt::READABLE
#define bbtco_emev_writeable bbt::pollevent::EventOpt::WRITEABLE
#define bbtco_emev_close    bbt::pollevent::EventOpt::CLOSE
#define bbtco_emev_finalize bbt::pollevent::EventOpt::FINALIZE
#define bbtco_emev_persist  bbt::pollevent::EventOpt::PERSIST

/**
 * @brief 在事件完成时，通过 scheduler 或指定 CoPool 安排后续处理
 *
 * 前置条件：scheduler 与 poller 已经可用。
 * 返回值：沿用事件注册返回码，0 为成功，负值为注册失败。
 *
 * 函数格式：bbtco_ev_[event type]、bbtco_ev_[event type]_with_copool
 *
 * with_copool版本会使用指定的协程池来注册事件。
 * 回调不会内联执行，而是被投递到新的协程任务或 CoPool worker 中执行。
 *
 * 支持以下事件：
 *  [w] 监听套接字可写
 *  [r] 监听套接字可读
 *  [c] 监听套接字关闭
 *  [t] 监听超时
 * 
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
 * @brief 等待指定的文件描述符事件
 *
 * 该函数只允许在协程上下文中使用，会挂起当前协程直到指定事件发生。
 * 若底层等待注册失败，会抛出 std::runtime_error。
 *
 * @param fd 文件描述符
 * @param event 事件类型，参考bbtco_emev_*
 * @param ms 超时时间，单位毫秒
 */
#define bbtco_wait_for(fd, event, ms) \
    bbt::coroutine::_WaitForHelper __bbtco_joint_suffix(_WaitForHelper_, __COUNTER__){fd, event, ms}
