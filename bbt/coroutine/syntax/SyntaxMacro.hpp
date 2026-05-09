#pragma once
#include <bbt/coroutine/syntax/_CoHelper.hpp>
#include <bbt/coroutine/syntax/_DeferHelper.hpp>
#include <bbt/coroutine/syntax/_EventHelper.hpp>
#include <bbt/coroutine/syntax/_WaitForHelper.hpp>

/* 辅助宏 */

/**
 * @brief 注册一个协程任务
 * 
 * 协程任务一旦注册，会在任意时刻在任意线程中被执行
 */
#define bbtco bbt::coroutine::_CoHelper()-

/**
 * @brief 注册一个协程任务，默认使用引用捕获，其他和bbtco相同
 */
#define bbtco_ref bbtco [&]()

/**
 * @brief 注册一个协程任务，并且在注册时记录协程是否注册成功无异常
 */
#define bbtco_noexcept(succ) bbt::coroutine::_CoHelper(succ)+ 

/**
 * @brief 注册一个协程任务，实际上和bbtco一致，只是增强了可读性
 */
#define bbtco_desc(desc) bbtco

/**
 * @brief 在协程中调用，使协程挂起一定时间后被唤醒
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
 * @brief 将当前协程挂起。协程立即进入就绪态，可能在任意时刻被唤醒
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
 * @brief 函数可以在事件完成的时候，新建一个协程来处理事件或者copool中处理事件
 * 
 * 函数格式：bbtco_ev_[event type]、bbtco_ev_[event type]_with_copool
 * 
 * with_copool版本会使用指定的协程池来注册事件。
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
 * 该函数会在协程中挂起，直到指定的文件描述符发生指定的事件。
 * 
 * @param fd 文件描述符
 * @param event 事件类型，参考bbtco_emev_*
 * @param ms 超时时间，单位毫秒
 */
#define bbtco_wait_for(fd, event, ms) \
    bbt::coroutine::_WaitForHelper __bbtco_joint_suffix(_WaitForHelper_, __COUNTER__){fd, event, ms}

