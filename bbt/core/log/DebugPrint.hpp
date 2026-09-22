#pragma once
#include <stdio.h>
#include <string>
#include <cstring>
#include <bbt/core/Define.hpp>

namespace bbt::core::log
{
// print使用buffer长度最大不超过此值
#define LOG_BUFFER_MAX_LEN ARRAY_SIZE

void WarnPrint(const char* fmt, ...);

void DebugPrint(const char* fmt, ...);

} // namespace bbt::core::log
