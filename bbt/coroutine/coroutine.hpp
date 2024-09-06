#pragma once

#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/syntax/_CoHelper.hpp>
#include <bbt/coroutine/syntax/_DeferHelper.hpp>
#include <bbt/coroutine/syntax/_EventHelper.hpp>

#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
#include <bbt/coroutine/sync/CoRWMutex.hpp>

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

/* event */
#define bbtco_event_regist(fd, event, time) (*std::make_shared<bbt::coroutine::_EventHelper>(fd, event, time))-

namespace bbt::coroutine
{

/* 获取当前运行的协程id，0为main线程中获取的id */
detail::CoroutineId GetLocalCoroutineId();

template<class TItem, int ItemMax>
inline typename sync::Chan<TItem, ItemMax>::SPtr Chan()
{ return std::make_shared<bbt::coroutine::sync::Chan<TItem, ItemMax>>(); }

}