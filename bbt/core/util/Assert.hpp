#pragma once
#include <cassert>
#include <iostream>
#include <bbt/core/Define.hpp>



/////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////

#if defined(NDEBUG)
#define DebugAssertWithInfo(expr, info) \
    assert( (expr) && (info) )
#else
#define DebugAssertWithInfo(expr, info) {}
#endif

#if defined(NDEBUG)
#define DebugAssert(expr) \
    assert( expr )
#else
#define DebugAssert(expr) {}
#endif

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
