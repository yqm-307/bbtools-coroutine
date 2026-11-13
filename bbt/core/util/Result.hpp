/**
 * @file Result.hpp
 * @brief Result<T, E>：值/错误二态返回类型，用一个返回值同时表达成功和失败。
 */
#pragma once
#include <variant>
#include <type_traits>

namespace bbt::core::util {

/**
 * @brief 二选一容器：要么持有 T（成功），要么持有 E（失败）。
 * 构造只接受右值（传具名变量要 std::move），T 与 E 必须不同类型。取错分支时 std::get 抛
 * std::bad_variant_access；Ok()/Err() 的 const& 重载可在临时对象上调用，返回引用不得越过该临时对象寿命。
 */
template <typename T, typename E> class Result {
    static_assert(!std::is_same_v<T, E>, "T and E must be different types");

public:
    /**
     * @brief 以成功值构造（移动进内部 variant）。
     */
    Result(T&& value);

    /**
     * @brief 以错误值构造（移动进内部 variant）。
     */
    Result(E&& error);

    /**
     * @brief 当前是否持有 T。
     */
    bool IsOk() const;

    /**
     * @brief 当前是否持有 E。
     */
    bool IsErr() const;

    /**
     * @brief 取成功值；错误态时抛 std::bad_variant_access。
     */
    T& Ok() &;

    /**
     * @brief 取错误值；成功态时抛 std::bad_variant_access。
     */
    E& Err() &;

    /**
     * @brief 只读取成功值。
     */
    const T& Ok() const&;

    /**
     * @brief 只读取错误值。
     */
    const E& Err() const&;

private:
    /**
     * 由 variant 的下标记录当前是哪一态
     */
    std::variant<T, E> m_value;
};

}

#include <bbt/core/util/__TResult.hpp>
