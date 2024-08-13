#include <bbt/coroutine/utils/DebugPrint.hpp>
#include <stdarg.h>

namespace bbt::coroutine::utils
{

void VPrint(const char* fmt, ...)
{
#ifdef BBT_COROUTINE_DEBUG_PRINT
    va_list args;

    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
#endif
}

}