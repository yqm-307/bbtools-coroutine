#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace bbt::coroutine::detail
{

class AwaitHelper;

// 仅给返回值附加类型标记；不持有任务、调度状态或额外堆内存。
template<class T>
class [[nodiscard]] AsyncReturn
{
    static_assert(!std::is_reference_v<T>, "async supports values and lvalue references, not rvalue references");
    friend class AwaitHelper;

public:
    template<class U, std::enable_if_t<std::is_convertible_v<U&&, T>, int> = 0>
    AsyncReturn(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)
        : m_value(std::forward<U>(value))
    {}

private:
    T m_value;
};

template<class T>
class [[nodiscard]] AsyncReturn<T&>
{
    friend class AwaitHelper;

public:
    // 只借用类型兼容的左值，拒绝临时对象及会产生临时量的隐式转换。
    template<class U, std::enable_if_t<std::is_convertible_v<U*, T*>, int> = 0>
    AsyncReturn(U& value) noexcept : m_value(std::addressof(value)) {}

private:
    T* m_value;
};

template<>
class [[nodiscard]] AsyncReturn<void>
{};

class AwaitHelper
{
public:
    // C++17 的 ->* 运算符写法先求值左侧：环境检查先于右侧实参和函数体。
    // 不保存 TLS/worker 指针，右侧挂起后可以恢复到另一 worker。
    AwaitHelper();

    template<class T>
    T operator->*(AsyncReturn<T>&& result) const
        noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        // 值必须移出包装，不能返回指向临时包装内部的引用。
        return std::move(result.m_value);
    }

    template<class T>
    T& operator->*(AsyncReturn<T&>&& result) const noexcept
    {
        return *result.m_value;
    }

    void operator->*(AsyncReturn<void>&&) const noexcept {}
};

} // namespace bbt::coroutine::detail

// 可变参数保留 std::pair<A, B> 等类型中的逗号；返回类型由实现细节包装。
#define bbt_co_async(...) ::bbt::coroutine::detail::AsyncReturn<__VA_ARGS__>
#define bbt_co_await ::bbt::coroutine::detail::AwaitHelper{} ->*
