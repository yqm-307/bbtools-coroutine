#include <stdarg.h>
#include <bbt/core/log/DebugPrint.hpp>

namespace bbt::core::log {

#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

namespace {

/**
 * 内部链接的格式化实现：与 bbtools-infra 侧同名符号隔离，避免动态链接器的
 * 符号插入把两边的实现互换。
 */
std::string vformat(const char* fmt, va_list ap)
{
    char data[ARRAY_SIZE];
    vsnprintf(data, sizeof(data), fmt, ap);

    return std::string(data);
}

}

/**
 * 内部辅助：头文件里没有声明，只在本编译单元使用（外部链接，链接器仍可见）。
 */
void VPrint(const char* fmt, const char* msg)
{
    /**
     * 缓冲比正文上限多留 10 字节，给前缀留位置；否则前缀会把正文尾部挤掉
     */
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

}
