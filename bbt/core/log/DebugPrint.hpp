/**
 * @file DebugPrint.hpp
 * @brief 最简调试输出：printf 风格格式化加级别前缀写到 stdout，不接日志系统、没有级别开关。
 */
#pragma once
#include <stdio.h>
#include <string>
#include <cstring>
#include <bbt/core/Define.hpp>

namespace bbt::core::log {

/**
 * @brief 单条打印消息的字节上限（4 KiB），由 `ARRAY_SIZE` 决定
 */
#define LOG_BUFFER_MAX_LEN ARRAY_SIZE

/**
 * @brief 打印一条告警到 stdout，前缀固定 `[ERROR]`；超过 `LOG_BUFFER_MAX_LEN` 截断，打印后立即 fflush
 */
void WarnPrint(const char* fmt, ...);

/**
 * @brief 打印一条调试信息到 stdout，前缀固定 `[DEBUG]`；截断与刷新行为同 `WarnPrint`
 */
void DebugPrint(const char* fmt, ...);

}
