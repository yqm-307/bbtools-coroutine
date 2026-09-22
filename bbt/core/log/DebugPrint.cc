#include <stdarg.h>
#include <bbt/core/log/DebugPrint.hpp>

namespace bbt::core::log
{

#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

namespace
{

/* vformat 原与 Logger 同编译单元（core/log/Logger.cc）。Logger 归
 * bbtools-infra 不随迁，这里只保留 DebugPrint 实际需要的最小实现；
 * 内部链接避免与 infra 侧同名导出符号在共存期互相插入。 */
std::string vformat(const char* fmt, va_list ap)
{
    char        data[ARRAY_SIZE];
    vsnprintf(data, sizeof(data), fmt, ap);

    return std::string(data);
}

} // namespace

void VPrint(const char* fmt, const char* msg)
{
    // 防止由于前缀导致的丢失数据
    char buf[LOG_BUFFER_MAX_LEN + 10];
    snprintf(buf, LOG_BUFFER_MAX_LEN, fmt, msg);
    printf("%s\n", buf);
    fflush(stdout);
};

void WarnPrint(const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    auto msg = vformat(fmt, ap);
    va_end(ap);

    VPrint("[ERROR] %s", msg.c_str());
}

void DebugPrint(const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    auto msg = vformat(fmt, ap);
    va_end(ap);

    VPrint("[DEBUG] %s", msg.c_str());
}

} // namespace bbt::core::log
