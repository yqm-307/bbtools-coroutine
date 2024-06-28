#pragma once

#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/_CoHelper.hpp>


#define bbtco bbt::coroutine::_CoHelper()-
#define bbtco_sleep(ms) bbt::coroutine::detail::Hook_Sleep(ms)


namespace bbt::coroutine
{

/* 获取当前运行的协程id，0为main线程中获取的id */
detail::CoroutineId GetLocalCoroutineId();

}