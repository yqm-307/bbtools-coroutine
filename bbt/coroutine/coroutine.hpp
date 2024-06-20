#pragma once

#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Hook.hpp>



namespace bbt::coroutine
{

/* 获取当前运行的协程id，0为main线程中获取的id */
detail::CoroutineId GetLocalCoroutineId();

}