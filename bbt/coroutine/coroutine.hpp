/**
 * @file coroutine.hpp
 * @brief 协程库公共入口、当前协程查询与常用同步组件。
 *
 * 使用本库只需要 `#include <bbt/coroutine/coroutine.hpp>`，它已经带入调度器、Hook、
 * 同步原语（Chan / CoCond / CoMutex / CoRWMutex）、协程池、CoObject 和语法糖。
 *
 * 从哪里读起：
 *  - 启动协程、休眠、让出、defer、等 fd 事件等写法：读 `syntax/SyntaxMacro.hpp` 文件头的
 *    速查表和每个宏的说明。同一意图优先用语法糖，不要直接调用 Scheduler / Coroutine 内部接口。
 *  - 声明和调用会挂起的函数：`bbt_co_async(T)` / `bbt_co_await`，见 `syntax/Async.hpp`。
 *  - 同步原语、协程池的完整签名与返回码：`docs/guides/api-reference.md`；
 *    运行时启动与生命期：`docs/guides/user-guide.md`。
 */
#pragma once

#include <bbt/coroutine/runtime/RuntimeMain.hpp>
#include <bbt/coroutine/syntax/RuntimeMacros.hpp>
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

namespace bbt {

namespace coroutine {

/**
 * @brief 只读查询当前执行协程 id；非协程上下文返回 0，不创建 Processer 或启动 runtime。
 */
detail::CoroutineId GetLocalCoroutineId();

/**
 * @brief 返回当前协程可用栈字节数；非协程上下文返回 0，同样不创建 Processer 或启动 runtime。
 */
size_t GetLocalCoroutineStackSize();

/**
 * @brief 无副作用查询当前执行流是否在协程上下文。
 */
bool IsInCoroutineContext();

/**
 * @brief 要求协程上下文；不在协程内时抛 `std::logic_error`。
 */
void RequireCoroutineContext();

/**
 * @brief 创建指定容量的 `Chan` 并返回共享 owner。
 * @tparam TItem 通道元素类型
 * @tparam ItemMax 缓冲容量
 * @return 新建通道的 shared_ptr
 */
template <class TItem, int ItemMax> inline typename sync::Chan<TItem, ItemMax>::SPtr Chan()
{
    return std::make_shared<bbt::coroutine::sync::Chan<TItem, ItemMax>>();
}

}

namespace co = coroutine;
}