#pragma once

#include <bbt/coroutine/detail/Scheduler.hpp>

#define g_scheduler bbt::coroutine::detail::Scheduler::GetInstance()

/* 当前线程正在运行的 coroutine */
#define g_bbt_coroutine_co (bbt::coroutine::detail::Processer::GetLocalProcesser()->GetCurrentCoroutine());

namespace bbt::coroutine
{

detail::CoroutineId GetLocalCoroutineId();

}