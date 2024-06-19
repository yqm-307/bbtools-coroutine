#pragma once

#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Hook.hpp>



namespace bbt::coroutine
{

detail::CoroutineId GetLocalCoroutineId();

}