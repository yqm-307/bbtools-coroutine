#pragma once
#include <variant>
#include <type_traits>

namespace bbt::core::util
{
    
template<typename T, typename E>
class Result {
    static_assert(!std::is_same_v<T, E>, "T and E must be different types");
public:
    Result(T&& value) : m_value(std::move(value)) {}
    Result(E&& error) : m_value(std::move(error)) {}
    
    bool IsOk() const { return !IsErr(); }
    bool IsErr() const { return std::holds_alternative<E>(m_value); }
    
    T& Ok() & { return std::get<T>(m_value); }
    E& Err() & { return std::get<E>(m_value); }

    const T& Ok() const& { return std::get<T>(m_value); }
    const E& Err() const& { return std::get<E>(m_value); }

private:
    std::variant<T, E> m_value;
};

} //  namespace bbt::core::util