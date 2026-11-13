#include <bbt/coroutine/utils/DebugPrint.hpp>
#include <stdarg.h>

namespace bbt::coroutine::utils {

/**
 * 宏关闭时这里整体编译为空函数：调用点已经展开成空，VPrint 只是保留符号，
 * 让直接调用它的代码（以及调试构建之外的链接）不受影响。
 */
void VPrint(const char* fmt, ...)
{
#ifdef BBT_COROUTINE_DEBUG_PRINT
    va_list args;

    va_start(args, fmt);

    /**
     * 直接写 stdout 并 flush：崩溃后仍能在终端看到最后几行，不经过 iostream 缓冲层
     */
    vfprintf(stdout, fmt, args);
    va_end(args);
    fflush(stdout);
#endif
}

}
