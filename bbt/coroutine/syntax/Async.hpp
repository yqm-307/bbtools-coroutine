/**
 * @file Async.hpp
 * @brief `bbt_co_async` / `bbt_co_bind` / `bbt_co_await` / `bbt_co_fire` 的异步操作契约。
 *
 * async API 返回惰性的 AsyncOperation。普通调用只构造操作描述，只有 await 或 fire
 * 入口能够执行它，避免把可能挂起的调用写成普通同步调用。
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
 * async 函数的公开返回类型；用 `bbt_co_bind` 构造返回的惰性操作。
 */
#define bbt_co_async(...) ::bbt::coroutine::detail::AsyncOperation<__VA_ARGS__>

/**
 * async 操作体的内部结果类型，只能在 async 操作的延迟执行体中使用。
 */
#define bbt_co_async_body(...) ::bbt::coroutine::detail::AsyncReturn<__VA_ARGS__>

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
