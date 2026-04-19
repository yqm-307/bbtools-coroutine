#pragma once

#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Hook.hpp>

#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
#include <bbt/coroutine/sync/CoRWMutex.hpp>
#include <bbt/coroutine/sync/StdLockWapper.hpp>

#include <bbt/coroutine/pool/CoPool.hpp>

#include <bbt/coroutine/syntax/SyntaxMacro.hpp>

/*
 * Public runtime contract
 *
 * 1. Configure detail::GlobalConfig before the first g_scheduler->Start().
 * 2. Start the scheduler before registering coroutines, creating CoPool workers,
 *    or using syntax helpers that suspend/resume coroutines.
 * 3. APIs that depend on coroutine-local state only have meaningful results on a
 *    scheduler worker thread while a coroutine is running.
 * 4. g_scheduler->Stop() is a lifecycle API for non-worker threads; calling it
 *    from coroutine worker context raises std::runtime_error.
 */

namespace bbt
{

namespace coroutine
{

/*
 * 获取当前运行的协程 id。
 * 在非协程上下文或非调度 worker 线程中返回 0。
 */
detail::CoroutineId GetLocalCoroutineId();

/*
 * 获取当前运行协程的栈大小。
 * 在非协程上下文或非调度 worker 线程中返回 0。
 */
size_t GetLocalCoroutineStackSize();

template<class TItem, int ItemMax>
inline typename sync::Chan<TItem, ItemMax>::SPtr Chan()
{ return std::make_shared<bbt::coroutine::sync::Chan<TItem, ItemMax>>(); }

} // namespace coroutine


namespace co = coroutine;
}