/**
 * @file __TAsync.hpp
 * @brief Async.hpp 的参数绑定实现。
 */
#pragma once

#include <tuple>
#include <bbt/coroutine/syntax/Async.hpp>

namespace bbt::coroutine::detail {

template <class F, class... Args, class Result, class>
AsyncOperation<Result> BindAsyncOperation(F&& callable, Args&&... args)
{
    return MakeAsyncOperation<Result>(
        [fn = std::decay_t<F>(std::forward<F>(callable)),
         saved = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...)]() mutable -> AsyncReturn<Result> {
            if constexpr (std::is_void_v<Result>) {
                std::apply(std::move(fn), std::move(saved));
                return {};
            } else {
                return std::apply(std::move(fn), std::move(saved));
            }
        });
}

} // namespace bbt::coroutine::detail
