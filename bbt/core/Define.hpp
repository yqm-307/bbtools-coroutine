/**
 * @file Define.hpp
 * @brief 全库共用的编译期宏与常量：分支预测、实现体声明、时间常量类型、标准版本检查。
 */
#pragma once

/**
 * C
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/**
 * CPP
 */
#include <memory>
#include <chrono>
#include <vector>
#include <thread>
#include <mutex>
#include <assert.h>
#include <queue>
#include <string>

/**
 * @brief 分支预测提示：`expr` 大概率为真
 */
#define bbt_likely(expr)    (__builtin_expect(((expr) != 0), 1))

/**
 * @brief 分支预测提示：`expr` 大概率为假
 */
#define bbt_unlikely(expr)  (__builtin_expect(((expr) != 0), 0))

/**
 * @brief 在类体内展开成 `private: struct`；后续成员因此变成 private，要显式写回 `public:`
 */
#define BBT_IMPL_STRUCT private:struct

/**
 * @brief 同 `BBT_IMPL_STRUCT`，展开成 `private: class`
 */
#define BBT_IMPL_CLASS  private:class

/**
 * @brief 头文件内定义常量用：`static inline constexpr uint64_t`，`inline` 避免多 TU 重复定义
 */
#define BBT_TIME_CONVERT_TYPE static inline constexpr uint64_t

/**
 * @brief 空展开的弃用标记，不产生告警；要告警用 `BBTATTR_FUNC_DEPRECATED`
 */
#define Deprecate

/**
 * @brief 全库宏与常量的集中定义处；`namespace bbt` 只给常量一个逻辑归属，宏展开不受它限制
 */

namespace bbt {

/**
 * @brief 调试开关；本仓当前没有使用点
 */
#define BBT_DEBUG

/**
 * @brief 预留的默认档位；本仓当前没有使用点
 */
#define ARRAY_NUM 8

/**
 * @brief 日志与格式化缓冲区的字节数上限（4 KiB），超长内容会被截断
 */
#define ARRAY_SIZE 1024*4

}

/**
 * 支持范围是 C++11 到 C++17，超出就编译失败。本库按 C++17 的标准库行为编写与测试，
 * 更老的标准缺当前用法，更新的标准没有验证过，不在这里悄悄放过。
 */
#if __cplusplus < 201103L || __cplusplus > 201703L
#error "versions greater than c++17 and smaller c++11 are not supported!"
#endif
