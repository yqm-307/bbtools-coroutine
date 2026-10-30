/* 协程库对外入口：汇总公共头，提供当前协程只读查询与 Chan 便捷构造。 */
#pragma once

#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Hook.hpp>

#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/sync/CoSelect.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
#include <bbt/coroutine/sync/CoRWMutex.hpp>
#include <bbt/coroutine/sync/StdLockWapper.hpp>

#include <bbt/coroutine/pool/CoPool.hpp>

#include <bbt/coroutine/syntax/SyntaxMacro.hpp>

namespace bbt
{

namespace coroutine
{

/* 只读查询当前执行协程；非协程上下文返回 0，不创建 Processer 或启动 runtime。 */
detail::CoroutineId GetLocalCoroutineId();
/* 返回当前协程可用栈字节数；非协程上下文返回 0，同样不创建 Processer 或启动 runtime。 */
size_t GetLocalCoroutineStackSize();

template<class TItem, int ItemMax>
inline typename sync::Chan<TItem, ItemMax>::SPtr Chan()
{ return std::make_shared<bbt::coroutine::sync::Chan<TItem, ItemMax>>(); }

} // namespace coroutine


namespace co = coroutine;
}