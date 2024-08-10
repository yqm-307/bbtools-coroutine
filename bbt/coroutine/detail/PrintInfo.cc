#include <stdarg.h>
#include <bbt/base/Logger/Logger.hpp>
#include <bbt/coroutine/detail/PrintInfo.hpp>

namespace bbt::coroutine::detail
{

void VPrintln(const char* fmt, const char* msg)
{
    char *buf = new char[BBT_COROUTINE_PRINTINFO_OUTPUT_MAX_SIZE + 10];
    snprintf(buf, BBT_COROUTINE_PRINTINFO_OUTPUT_MAX_SIZE, fmt, msg);
    printf("%s\n", buf);
    fflush(stdout);
    delete[] buf;
}

void __DebugPrint(const char* fmt, ...)
{
#ifdef BBT_COROUTINE_ENABLE_DEBUG_PRINT
    va_list ap;
    va_start(ap, fmt);
    char *msg = new char[BBT_COROUTINE_PRINTINFO_OUTPUT_MAX_SIZE];
    memset(msg, '\0', BBT_COROUTINE_PRINTINFO_OUTPUT_MAX_SIZE);
    vsnprintf(msg, BBT_COROUTINE_PRINTINFO_OUTPUT_MAX_SIZE, fmt, ap);
    va_end(ap);

    VPrintln("[DEBUG] %s", msg);
    delete[] msg;
#endif
}

void __WarnPrint(const char* fmt, ...)
{
#ifdef BBT_COROUTINE_ENABLE_DEBUG_PRINT
    va_list ap;
    va_start(ap, fmt);
    char *msg = new char[BBT_COROUTINE_PRINTINFO_OUTPUT_MAX_SIZE];
    memset(msg, '\0', BBT_COROUTINE_PRINTINFO_OUTPUT_MAX_SIZE);
    vsnprintf(msg, BBT_COROUTINE_PRINTINFO_OUTPUT_MAX_SIZE, fmt, ap);
    va_end(ap);

    VPrintln("[WARN] %s", msg);
    delete[] msg;
#endif
}

}