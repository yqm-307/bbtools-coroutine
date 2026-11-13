/**
 * @file __TCoEventValue.hpp
 * @brief CoEventValue 的成员实现，由 CoEventValue.hpp 末尾包含。
 */
#pragma once
#include <bbt/coroutine/sync/CoEventValue.hpp>

namespace bbt::coroutine {

inline bool CoEventValue::HasValue() const noexcept
{
    return m_type != kCoEventValueNone;
}

template <class T> CoEventValue CoEventValue::From(const T& value) noexcept
{
    using Type = std::remove_cv_t<T>;
    static_assert(std::is_trivially_copyable<Type>::value, "CoEventValue: only trivially copyable types are supported");
    static_assert(sizeof(Type) <= kInlineSize, "CoEventValue: payload exceeds the 24-byte inline capacity");

    CoEventValue out;
    out.m_type = CoEventValueTraits<Type>::id;
    out.m_size = static_cast<std::uint8_t>(sizeof(Type));
    std::memcpy(out.m_buf, &value, sizeof(Type));
    return out;
}

template <class T> bool CoEventValue::Get(T& out) const noexcept
{
    using Type = std::remove_cv_t<T>;
    static_assert(std::is_trivially_copyable<Type>::value, "CoEventValue: only trivially copyable types are supported");
    static_assert(sizeof(Type) <= kInlineSize, "CoEventValue: payload exceeds the 24-byte inline capacity");

    if (m_type == kCoEventValueNone || m_type != CoEventValueTraits<Type>::id || m_size != sizeof(Type))
        return false;

    std::memcpy(&out, m_buf, sizeof(Type));
    return true;
}

}
