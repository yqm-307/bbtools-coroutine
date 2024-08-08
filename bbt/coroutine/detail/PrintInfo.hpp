#pragma once
#include <bbt/base/Logger/DebugPrint.hpp>

#define BBT_COROUTINE_PRINTINFO_OUTPUT_MAX_SIZE 10240

namespace bbt::coroutine::detail
{

void __DebugPrint(const char* fmt, ...);
void __WarnPrint(const char* fmt, ...);

}

#define __bbt_warn_print_detail(msg, ...)       (bbt::coroutine::detail::__WarnPrint(msg,  ##__VA_ARGS__))
#define __bbt_debug_print_detail(msg, ...)      (bbt::coroutine::detail::__DebugPrint(msg, ##__VA_ARGS__))

#define g_bbt_sys_warn_print \
    __bbt_warn_print_detail("[%s:%d %s], errno: %d [%s]", __FILE__, __LINE__, __FUNCTION__, errno, strerror(errno))

#define g_bbt_warn_print_with_errno(msg) \
    __bbt_warn_print_detail("[%s:%d %s], errno: %d [%s], %s", __FILE__, __LINE__, __FUNCTION__, errno, strerror(errno), msg)

#define g_bbt_warn_print(msg, ...) \
    __bbt_warn_print_detail(msg,  ##__VA_ARGS__)

#define g_bbt_debug_print(msg, ...) \
    __bbt_debug_print_detail(msg, ##__VA_ARGS__)

#define g_bbt_warn_print_with_lib_flag(msg) \
    __bbt_warn_print_detail("[bbt-coroutine] %s", msg)

#define g_bbt_debug_print_with_lib_flag(msg) \
    __bbt_debug_print_detail("[bbt-coroutine] %s", msg)
