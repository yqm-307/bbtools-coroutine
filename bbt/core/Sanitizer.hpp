/**
 * @file Sanitizer.hpp
 * @brief 在 GCC 和 Clang 上统一检测 ThreadSanitizer 构建。
 */
#pragma once

#if defined(__SANITIZE_THREAD__)
#define BBT_TSAN_ENABLED 1
#elif defined(__has_feature)
// 必须在单独的预处理分支里调用，GCC 13 不能解析未定义的 __has_feature(...)。
#if __has_feature(thread_sanitizer)
#define BBT_TSAN_ENABLED 1
#endif
#endif
