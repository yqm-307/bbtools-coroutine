#pragma once

#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/_CoHelper.hpp>


#define bbtco bbt::coroutine::_CoHelper()-
#define bbtco_desc(desc) bbtco
#define bbtco_sleep(ms) bbt::coroutine::detail::Hook_Sleep(ms)
#define co_desc(desc_msg)


namespace bbt::coroutine
{

/* 获取当前运行的协程id，0为main线程中获取的id */
detail::CoroutineId GetLocalCoroutineId();

template<class TItem, int ItemMax>
inline typename sync::Chan<TItem, ItemMax>::SPtr Chan()
{ return std::make_shared<bbt::coroutine::sync::Chan<TItem, ItemMax>>(); }

}