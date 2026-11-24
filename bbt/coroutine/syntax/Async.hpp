/**
 * @file Async.hpp
 * @brief `bbt_co_async` / `bbt_co_bind` / `bbt_co_await` / `bbt_co_fire` 的异步操作契约。
 *
 * async API 同时支持两种入口。`bbt_co_bind` 返回惰性的 AsyncOperation，顺序 async
 * 函数体则在函数调用时执行，并在 `bbt_co_return` 处包装最终结果。
 */
#pragma once

#include <bbt/coroutine/detail/Scheduler.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bbt::coroutine::detail {

template <class T> class AsyncReturn;
class AwaitHelper;
class FireHelper;

template <class T> class [[nodiscard]] AsyncReturn {
    static_assert(!std::is_reference_v<T>, "async supports values and lvalue references, not rvalue references");
    friend class AwaitHelper;

public:
    template <class U, std::enable_if_t<std::is_convertible_v<U&&, T>, int> = 0>
    AsyncReturn(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)
        : m_value(std::forward<U>(value))
    {
    }

private:
    T m_value;
};

template <class T> class [[nodiscard]] AsyncReturn<T&> {
    friend class AwaitHelper;

public:
    template <class U, std::enable_if_t<std::is_convertible_v<U*, T*>, int> = 0>
    AsyncReturn(U& value) noexcept
        : m_value(std::addressof(value))
    {
    }

private:
    T* m_value;
};

template <> class [[nodiscard]] AsyncReturn<void> {};

/**
 * @brief 延迟执行一个 async 操作。
 *
 * 操作体是 move-only 的类型擦除对象。它不能直接调用，必须交给 AwaitHelper 或
 * FireHelper。Run 只能在 helper 获得操作后调用一次。
 */
template <class T> class [[nodiscard]] AsyncOperation {
    struct Body {
        virtual ~Body() = default;
        virtual AsyncReturn<T> Run() = 0;
    };

    template <class F> struct BodyImpl final : Body {
        explicit BodyImpl(F body)
            : m_body(std::move(body))
        {
        }

        AsyncReturn<T> Run() override { return m_body(); }

        F m_body;
    };

    friend class AwaitHelper;
    friend class FireHelper;

public:
    template <class F, std::enable_if_t<std::is_convertible_v<std::invoke_result_t<F&>, AsyncReturn<T>>, int> = 0>
    explicit AsyncOperation(F&& body)
        : m_body(std::make_unique<BodyImpl<std::decay_t<F>>>(std::forward<F>(body)))
    {
    }

    AsyncOperation(const AsyncOperation&) = delete;
    AsyncOperation& operator=(const AsyncOperation&) = delete;
    AsyncOperation(AsyncOperation&&) noexcept = default;
    AsyncOperation& operator=(AsyncOperation&&) noexcept = default;

private:
    AsyncReturn<T> Run() &&
    {
        if (!m_body) {
            throw std::logic_error("async operation already consumed");
        }
        auto body = std::move(m_body);
        return body->Run();
    }

    std::unique_ptr<Body> m_body;
};

template <class T, class F> AsyncOperation<T> MakeAsyncOperation(F&& body)
{
    return AsyncOperation<T>(std::forward<F>(body));
}

template <class T> AsyncOperation<std::decay_t<T>> MakeReadyAsync(T&& value)
{
    using Result = std::decay_t<T>;
    return MakeAsyncOperation<Result>(
        [value = std::forward<T>(value)]() mutable -> AsyncReturn<Result> { return std::move(value); });
}

/**
 * @brief 用赋值优先级接住返回表达式，包括三元表达式，但不吞掉顶层逗号。
 */
class ReturnHelper {
public:
    template <class T> AsyncOperation<std::decay_t<T>> operator=(T&& value) const
    {
        return MakeReadyAsync(std::forward<T>(value));
    }
};

template <class T> AsyncOperation<T&> MakeReadyAsyncRef(T& value)
{
    return MakeAsyncOperation<T&>([&value]() -> AsyncReturn<T&> { return value; });
}

inline AsyncOperation<void> MakeReadyAsyncVoid()
{
    return MakeAsyncOperation<void>([]() -> AsyncReturn<void> { return {}; });
}

template <
    class F,
    class... Args,
    class Result = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>,
    class = std::enable_if_t<
        !std::is_rvalue_reference_v<Result> && std::is_constructible_v<std::decay_t<F>, F&&>
        && (std::is_constructible_v<std::decay_t<Args>, Args&&> && ...)
        && std::is_move_constructible_v<std::decay_t<F>> && (std::is_move_constructible_v<std::decay_t<Args>> && ...)>>
AsyncOperation<Result> BindAsyncOperation(F&& callable, Args&&... args);

/**
 * @brief 执行当前协程中的 async 操作并解包结果。
 */
class AwaitHelper {
public:
    AwaitHelper();

    template <class T> T operator->*(AsyncReturn<T>&& result) const noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        return std::move(result.m_value);
    }

    template <class T> T& operator->*(AsyncReturn<T&>&& result) const noexcept { return *result.m_value; }

    void operator->*(AsyncReturn<void>&&) const noexcept {}

    template <class T> T operator->*(AsyncOperation<T>&& operation) const
    {
        if constexpr (std::is_void_v<T>) {
            AwaitHelper{}->*std::move(operation).Run();
        } else {
            return AwaitHelper{}->*std::move(operation).Run();
        }
    }
};

/**
 * @brief 提交 async 操作到 detached 协程，不等待最终结果。
 */
class FireHelper {
public:
    template <class T> void operator->*(AsyncOperation<T>&& operation) const
    {
        auto shared_operation = std::make_shared<AsyncOperation<T>>(std::move(operation));
        if (!g_scheduler || !g_scheduler->IsInitialized()) {
            throw std::runtime_error("bbt_co_fire requires an initialized scheduler");
        }
        bool submitted = false;
        g_scheduler->RegistCoroutineTask(
            [shared_operation]() mutable { AwaitHelper{}->*std::move(*shared_operation); }, submitted);
        if (!submitted) {
            throw std::runtime_error("bbt_co_fire submission failed");
        }
    }
};

} // namespace bbt::coroutine::detail

#include <bbt/coroutine/syntax/__TAsync.hpp>

/**
 * async 函数的公开返回类型。顺序 async 函数体用 `bbt_co_return` 返回值，
 * 惰性操作用 `bbt_co_bind` 构造。
 */
#define bbt_co_async(...) ::bbt::coroutine::detail::AsyncOperation<__VA_ARGS__>

/**
 * async 操作体的内部结果类型，只能在 async 操作的延迟执行体中使用。
 */
#define bbt_co_async_body(...) ::bbt::coroutine::detail::AsyncReturn<__VA_ARGS__>

/**
 * @brief 按值返回顺序 async 函数的结果：`bbt_co_return value + 1;`。
 *
 * 函数体在调用时执行，只有返回值被包装为已完成的 AsyncOperation。
 * 顶层逗号表达式须加括号，例如 `bbt_co_return (first(), second());`。
 */
#define bbt_co_return return ::bbt::coroutine::detail::ReturnHelper{} =

/**
 * @brief 返回顺序 async 函数体的 void 结果：`bbt_co_return_void;`。
 */
#define bbt_co_return_void return ::bbt::coroutine::detail::MakeReadyAsyncVoid()

/**
 * @brief 返回借用的左值：`bbt_co_return_ref(value);`。对象须活到调用方最后一次使用引用。
 */
#define bbt_co_return_ref(...) return ::bbt::coroutine::detail::MakeReadyAsyncRef((__VA_ARGS__))

/**
 * @brief 绑定可调用对象和实参，推导 async 结果，不立即执行：`bbt_co_bind(callable, args...)`。
 *
 * 绑定时按值保存可移动的对象（左值拷贝、右值移动），消费时以右值调用。`std::ref` / `std::cref`、
 * 指针、视图和 lambda 引用捕获仍只借用；引用结果不能指向操作自身持有的数据。
 */
#define bbt_co_bind ::bbt::coroutine::detail::BindAsyncOperation

/**
 * 当前协程等待并取得 async 操作结果。
 */
#define bbt_co_await ::bbt::coroutine::detail::AwaitHelper{}->*

/**
 * 启动 detached async 操作，不等待完成结果。
 */
#define bbt_co_fire ::bbt::coroutine::detail::FireHelper{}->*
