/** @file coroutine.hpp
 * @brief 协程库公共入口、当前协程查询与常用同步组件。
 */
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

#include <bbt/coroutine/object/CoRuntimeOnly.hpp>
#include <bbt/coroutine/object/CoObject.hpp>

#include <bbt/coroutine/syntax/SyntaxMacro.hpp>

namespace bbt
{

namespace coroutine
{

/**
 * @brief 只读查询当前执行协程 id；非协程上下文返回 0，不创建 Processer 或启动 runtime。
 */
detail::CoroutineId GetLocalCoroutineId();
/**
 * @brief 返回当前协程可用栈字节数；非协程上下文返回 0，同样不创建 Processer 或启动 runtime。
 */
size_t GetLocalCoroutineStackSize();

/// @brief 无副作用查询当前执行流是否在协程上下文。
bool IsInCoroutineContext();
/// @brief 要求协程上下文；不在协程内时抛 `std::logic_error`。
void RequireCoroutineContext();

/**
 * @brief 创建指定容量的 `Chan` 并返回共享 owner。
 * @tparam TItem 通道元素类型
 * @tparam ItemMax 缓冲容量
 * @return 新建通道的 shared_ptr
 */
template<class TItem, int ItemMax>
inline typename sync::Chan<TItem, ItemMax>::SPtr Chan()
{ return std::make_shared<bbt::coroutine::sync::Chan<TItem, ItemMax>>(); }

} // namespace coroutine


namespace co = coroutine;
}