/**
 * @file Assert.hpp
 * @brief 断言宏：Debug 构建 abort，Release（NDEBUG）构建打印到 stderr 后继续，函数带着非法状态往下跑。
 * 表达式在两种构建下都会被求值，所以不要塞副作用（不要写 Assert(x++)）。
 */
#pragma once
#include <cassert>
#include <iostream>
#include <bbt/core/Define.hpp>

/**
 * @brief Debug 断言：两个构建分支都不检查、不求值 expr。
 */
#if defined(NDEBUG)
#define DebugAssertWithInfo(expr, info) \
    assert( (expr) && (info) )
#else
#define DebugAssertWithInfo(expr, info) {}
#endif

/**
 * @brief Debug 断言（无附加信息）：两个构建分支都不检查、不求值 expr。
 */
#if defined(NDEBUG)
#define DebugAssert(expr) \
    assert( expr )
#else
#define DebugAssert(expr) {}
#endif

/**
 * @brief 断言：Debug 构建 abort；Release 构建把 "Assertion failed: <expr> at <file>:<line>" 打到 stderr 后继续。
 * 表达式在两种构建下都会被求值；Release 分支用 do{...}while(0) 包住，可当普通语句带分号使用。
 */
#if defined(NDEBUG)
#define Assert(expr) \
    do { \
        if (bbt_unlikely(!(expr))) { \
            std::cerr << "Assertion failed: " << #expr << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        } \
    } while(0)
#else
#define Assert(expr) \
    assert( expr )
#endif

/**
 * @brief 带附加信息的断言：Release 构建额外打印 (info) 后继续；Debug 构建展开成 assert((expr) && (info))，失败即 abort。
 * Debug 分支把 info 放进 assert 的布尔表达式，所以它必须能隐式转成 bool——std::string 在 Debug 下编译不过。
 */
#if defined(NDEBUG)
#define AssertWithInfo(expr, info) \
    do { \
        if (bbt_unlikely(!(expr))) { \
            std::cerr << "Assertion failed: " << #expr << " (" << info << ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
        } \
    } while(0)
#else
#define AssertWithInfo(expr, info) \
    assert( (expr) && (info) )
#endif
