#pragma once
#include <iostream>


namespace bbt::coroutine::utils
{

void VPrint(const char* fmt, ...);

#ifdef BBT_COROUTINE_DEBUG_PRINT

#define __g_bbt_debug_printf(fmt, ...) \
    bbt::coroutine::utils::VPrint(fmt, ##__VA_ARGS__)

#define __g_bbt_debug_printf_full(msg) \
    __g_bbt_debug_printf("[%s:%d %s] %s\n", __FILE__, __LINE__, __FUNCTION__, msg)

#define g_bbt_dbgp(module, msg) \
    __g_bbt_debug_printf("[%s:%d %s] [module:%s] %s\n", __FILE__, __LINE__, __FUNCTION__, module, msg);

#define g_bbt_dbgp_full(msg) \
    __g_bbt_debug_printf_full(msg)

#else
#define __g_bbt_debug_printf(fmt, ...)

#define __g_bbt_debug_printf_full(msg)

#define g_bbt_dbgp(module, msg)

#define g_bbt_dbgp_full(msg)
#endif

}