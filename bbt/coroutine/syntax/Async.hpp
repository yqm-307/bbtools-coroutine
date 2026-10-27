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

// 声明端标记：把返回类型包成 AsyncReturn<T>，调用方必须经 bbt_co_await 解包。
// 可变参数保留 std::pair<A, B> 等类型中的逗号。
//
// 它只改返回类型，不插入任何检查，所以下面三条靠编译器诊断兜底：
//  - 虚函数写成 `virtual bbt_co_async(T) F() = 0;`，virtual 不能丢，否则
//    报 "initializer specified for non-virtual method"。
//  - bbt_co_async(void) 的函数必须显式 `return {};`；走到函数末尾是未定义行为，
//    编译器默认只给 -Wreturn-type 警告，调用方构建必须开 -Werror=return-type。
//  - 漏写 bbt_co_await 时函数体仍会执行（只是结果被丢弃，且没有协程环境检查），
//    只有 [[nodiscard]] 的 -Wunused-result 警告，调用方构建必须开
//    -Werror=unused-result。
// 依赖本库的仓库（infra、framework 等）需在自己的 CMake 里
// add_compile_options(-Werror=return-type -Werror=unused-result)；本库顶层
// CMakeLists.txt 已开启。
#define bbt_co_async(...) ::bbt::coroutine::detail::AsyncReturn<__VA_ARGS__>

// 调用端标记：`AwaitHelper{} ->*`。借 C++17 对 ->* 左操作数先于右操作数的求值
// 顺序，让协程环境检查先于被调函数的实参与函数体；不在协程内抛 std::logic_error。
// 它不创建协程，也不能阻止别人绕过它直接调用 bbt_co_async 函数。
//
// 因为是 ->* 而不是关键字，优先级低于一元前缀运算符和强制转换：
//  - `!bbt_co_await f()`、`*bbt_co_await f()`、`(T)bbt_co_await f()` 会先作用于
//    AwaitHelper 而编译失败，写成 `!(bbt_co_await f())`。
//  - 取成员写 `(bbt_co_await f()).m`，不要写 `bbt_co_await f().m`。
//  - 不能嵌套 `bbt_co_await bbt_co_await f()`（AwaitHelper ->* AwaitHelper 无匹配）。
// 只能用于 bbt_co_async 声明的函数，普通值不能 await。
#define bbt_co_await ::bbt::coroutine::detail::AwaitHelper{} ->*
