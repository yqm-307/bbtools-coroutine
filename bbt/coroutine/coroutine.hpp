#pragma once

#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/_CoHelper.hpp>


#define bbtco bbt::coroutine::_CoHelper()-
#define bbtco_sleep(ms) bbt::coroutine::detail::Hook_Sleep(ms)


namespace bbt::coroutine
{

/* 获取当前运行的协程id，0为main线程中获取的id */
detail::CoroutineId GetLocalCoroutineId();

template<class TItem>
inline typename sync::Chan<TItem>::SPtr Chan(size_t max = 65535)
{ return std::make_shared<bbt::coroutine::sync::Chan<TItem>>(max); }

}