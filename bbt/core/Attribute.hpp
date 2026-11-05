/**
 * @file Attribute.hpp
 * @brief GCC 属性宏封装；每项先用 `__glibc_has_attribute` 探测，缺失则 `#error`。
 */
#pragma once
#include <sys/cdefs.h>

//---------------------------- 函数注解 ----------------------------//
/**
 * 函数注解需要加载函数声明的末尾
 */

/**
 * @brief 调用点忽略返回值时产生 `-Wunused-result` 告警，不是编译错误
 */
#if __glibc_has_attribute(warn_unused_result) >= 0
#define BBTATTR_FUNC_WARN_UNUSERESULT __attribute__((warn_unused_result))
#else
#error "not exist attribute [warn_unused_result]"
#endif

/**
 * @brief 冷函数提示，编译器据此把函数排到冷代码区；`-O0` 下基本无效
 */
#if __glibc_has_attribute(cold) >= 0
#define BBTATTR_FUNC_COLD __attribute__((cold))
#else
#error "not exist attribute [cold]"
#endif

/**
 * @brief 热函数提示，让编译器更积极地内联与布局；标错不会有错，也没有收益
 */
#if __glibc_has_attribute(hot) >= 0
#define BBTATTR_FUNC_HOT __attribute__((hot))
#else
#error "not exist attribute [hot]"
#endif

/**
 * @brief 标记弃用并附说明文字；调用点产生 `-Wdeprecated-declarations`，只有 `-Werror` 才编译失败
 */
#if __glibc_has_attribute(__deprecated__) >= 0
#define BBTATTR_FUNC_DEPRECATEDMSG(msg)    __attribute_deprecated_msg__(msg)
#else
#error "not exist attribute [__deprecated__]"
#endif

/**
 * @brief 标记弃用，不带说明文字；调用点只有一句默认的弃用告警
 */
#if __glibc_has_attribute(__deprecated__) >= 0
#define BBTATTR_FUNC_DEPRECATED         __attribute_deprecated__
#else
#error "not exist attribute [__deprecated__]"
#endif

/**
 * @brief 构造函数弃用告警；只是编译期警告，不是访问控制，隐式调用（拷贝、转换）照样编译
 */
#if __glibc_has_attribute(__deprecated__) >= 0
#define BBTATTR_FUNC_CTOR_HIDDEN    BBTATTR_FUNC_DEPRECATEDMSG("this constructor is hidden and must not be called directly")
#else
#error "not exist attribute [BBTATTR_FUNC_DEPRECATEDMSG]"
#endif

//---------------------------- 通用注解 ----------------------------//
/**
 * @brief 标记有意不使用的变量/参数/函数，压掉 `-Wunused-*` 告警
 */
#if __glibc_has_attribute(unused) >= 0
#define BBTATTR_COMM_UNUSED         __attribute__((unused))
#else
#error "not exist attribute [unused]"
#endif

/* 待补充的 GCC 属性（内存访问范围、fd 参数、nonnull 之类）等有实际调用点再加：
 * 先出现需要约束的函数，再引入对应宏；否则只会多一批没人用的封装。 */
