/**
 * @file __TResult.hpp
 * @brief Result 的模板实现，由 Result.hpp 末尾包含。
 */
#pragma once
#include <bbt/core/util/Result.hpp>

namespace bbt::core::util {

template <typename T, typename E> Result<T, E>::Result(T&& value) : m_value(std::move(value)) {}

template <typename T, typename E> Result<T, E>::Result(E&& error) : m_value(std::move(error)) {}

template <typename T, typename E> bool Result<T, E>::IsOk() const
{
    return !IsErr();
}

template <typename T, typename E> bool Result<T, E>::IsErr() const
{
    return std::holds_alternative<E>(m_value);
}

template <typename T, typename E> T& Result<T, E>::Ok() &
{
    return std::get<T>(m_value);
}

template <typename T, typename E> E& Result<T, E>::Err() &
{
    return std::get<E>(m_value);
}

template <typename T, typename E> const T& Result<T, E>::Ok() const&
{
    return std::get<T>(m_value);
}

template <typename T, typename E> const E& Result<T, E>::Err() const&
{
    return std::get<E>(m_value);
}

}
