/** @file DebugPrint.hpp @brief 条件编译的调试打印：BBT_COROUTINE_DEBUG_PRINT 开启才输出，关闭时宏展开为空。 */
#pragma once
#include <iostream>

namespace bbt::coroutine::utils {

/**
 * @brief 调试打印的唯一入口：vfprintf 到 stdout 后立即 flush（崩溃时最后几行还在）。
 *        宏关闭时函数体为空、调用不报错，所以调用点不必再套 #ifdef。
 */
void VPrint(const char* fmt, ...);

#ifdef BBT_COROUTINE_DEBUG_PRINT

/// @brief 格式化打印；开启调试时才展开成一次 VPrint 调用
#define __g_bbt_debug_printf(fmt, ...) \
    bbt::coroutine::utils::VPrint(fmt, ##__VA_ARGS__)

/// @brief 带 `[文件:行 函数]` 前缀的打印，用来定位调用点
#define __g_bbt_debug_printf_full(msg) \
    __g_bbt_debug_printf("[%s:%d %s] %s\n", __FILE__, __LINE__, __FUNCTION__, msg)

/// @brief 带模块名和调用点前缀的打印
#define g_bbt_dbgp(module, msg) \
    __g_bbt_debug_printf("[%s:%d %s] [module:%s] %s\n", __FILE__, __LINE__, __FUNCTION__, module, msg);

/// @brief 同 __g_bbt_debug_printf_full
#define g_bbt_dbgp_full(msg) \
    __g_bbt_debug_printf_full(msg)

#else
/* 关闭时四个宏都展开成空：实参不会被求值，带副作用的实参（自增、函数调用）
 * 在调试构建和发布构建里行为不同，调用点不要依赖它们。 */
#define __g_bbt_debug_printf(fmt, ...)

#define __g_bbt_debug_printf_full(msg)

#define g_bbt_dbgp(module, msg)

#define g_bbt_dbgp_full(msg)
#endif

}
