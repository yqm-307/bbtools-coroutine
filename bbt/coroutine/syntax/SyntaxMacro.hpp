#pragma once
#include <bbt/coroutine/syntax/_CoHelper.hpp>
#include <bbt/coroutine/syntax/_DeferHelper.hpp>
#include <bbt/coroutine/syntax/_EventHelper.hpp>

/* 辅助宏 */
#define bbtco bbt::coroutine::_CoHelper()-
#define bbtco_desc(desc) bbtco

#define bbtco_sleep(ms) bbt::coroutine::detail::Hook_Sleep(ms)

#define bbtco_concat_impl(x, y) x##y
#define bbtco_concat(x, y) bbtco_concat_impl(x, y)
#define bbtco_joint_suffix(name, suffix) bbtco_concat(name, suffix)

#define bbtco_defer_ex \
    bbt::coroutine::detail::Defer bbtco_joint_suffix(bbtco_defer_, __COUNTER__) = bbt::coroutine::_DeferHelper()-

#define bbtco_defer \
    bbtco_defer_ex [&]()

#define bbtco_yield \
do \
{ \
    if (g_bbt_tls_coroutine_co == nullptr) \
        break; \
    g_bbt_tls_coroutine_co->YieldAndPushGCoQueue(); \
} while(0);

/* sync */
#define bbtco_make_cocond(mtx)     bbt::coroutine::sync::CoCond::Create(mtx)
#define bbtco_make_comutex()    bbt::coroutine::sync::CoMutex::Create()
#define bbtco_make_corwmutex()  bbt::coroutine::sync::CoRWMutex::Create()
/* copool */
#define bbtco_make_copool(pool_max_co) bbt::coroutine::pool::CoPool::Create(pool_max_co)

/* event */
#define __bbtco_event_regist_ex(fd, event, time) (*std::make_shared<bbt::coroutine::_EventHelper>(fd, event, time))-
#define __bbtco_event_regist_with_copool_ex(fd, event, time, copool) (*std::make_shared<bbt::coroutine::_EventHelper>(fd, event, time, copool))-

#define bbtco_emev_timeout bbt::pollevent::EventOpt::TIMEOUT
#define bbtco_emev_readable bbt::pollevent::EventOpt::READABLE
#define bbtco_emev_writeable bbt::pollevent::EventOpt::WRITEABLE
#define bbtco_emev_close    bbt::pollevent::EventOpt::CLOSE
#define bbtco_emev_finalize bbt::pollevent::EventOpt::FINALIZE
#define bbtco_emev_persist  bbt::pollevent::EventOpt::PERSIST

/**
 * event regist helper
 * 
 * bbtco_ev_[event type]
 * 
 * event type:
 * [w] 监听套接字可读
 * [r] 监听套接字可写
 * [c] 监听套接字关闭
 * [t] 监听超时
 * 
 * 若不满足，可使用 __bbtco_event_regist_ex
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