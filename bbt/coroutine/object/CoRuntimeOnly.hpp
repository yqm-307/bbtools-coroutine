/**
 * @file CoRuntimeOnly.hpp
 * @brief Co-only 能力标记：仅显式入口检查，不约束对象构造、析构或句柄移动。
 */
#pragma once

namespace bbt::coroutine {

/**
 * @brief 无副作用查询当前执行流是否在协程上下文；不创建 Processer、不启动 runtime。
 */
bool IsInCoroutineContext();

/**
 * @brief 要求协程上下文；不在协程内时抛 `std::logic_error`。
 */
void RequireCoroutineContext();

/**
 * @brief Co-only 能力标记基类：派生类型只在显式入口要求协程上下文。
 *
 * 构造、析构与句柄移动都不检查执行域——标记只影响入口处的 `RequireCoroutineContext()`。
 */
class CoRuntimeOnly {
public:
    /**
     * @brief 同自由函数 `IsInCoroutineContext()`。
     */
    static bool IsInCoroutineContext() { return bbt::coroutine::IsInCoroutineContext(); }

    /**
     * @brief 同自由函数 `RequireCoroutineContext()`。
     */
    static void RequireCoroutineContext() { bbt::coroutine::RequireCoroutineContext(); }
};

}
