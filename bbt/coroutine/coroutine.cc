/**
 * @file coroutine.cc
 * @brief 协程上下文查询与入口检查；真源是 TLS 上的当前 Coroutine，只读解析、不创建 runtime。
 */
#include <bbt/coroutine/coroutine.hpp>
#include <stdexcept>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>

namespace bbt::coroutine {

/**
 * 检查放在构造函数里，是因为宏展开成 `AwaitHelper{} ->* f()`：C++17 保证左操作数
 * 先于右操作数求值，所以这里在 f() 的实参与函数体之前就抛出 std::logic_error。
 */
detail::AwaitHelper::AwaitHelper()
{
    if (GetLocalCoroutineId() == 0)
        throw std::logic_error("bbt_co_await requires a coroutine context");
}

bool IsInCoroutineContext()
{
    return GetLocalCoroutineId() != 0;
}

void RequireCoroutineContext()
{
    if (!IsInCoroutineContext())
        throw std::logic_error("operation requires a coroutine context");
}

/**
 * 三段解析：TLS 辅助对象 → 本地 Processer → 当前 Coroutine；任一段缺失都返回 0，
 * 不区分「没有 runtime」和「在 runtime 里但不在协程中」。
 */
detail::CoroutineId GetLocalCoroutineId()
{
    auto local = detail::LocalThread::TryGetTLSInst();
    if (local == nullptr || !local->EnableUseCo())
        return 0;

    auto tls_processer = detail::Processer::TryGetLocalProcesser();
    if (tls_processer == nullptr)
        return 0;

    auto coroutine = tls_processer->GetCurrentCoroutine();
    if (coroutine == nullptr)
        return 0;

    return coroutine->GetId();
}

/**
 * 取当前 Coroutine 自己的栈大小（各协程可不同），不是配置默认值。
 */
size_t GetLocalCoroutineStackSize()
{
    auto local = detail::LocalThread::TryGetTLSInst();
    if (local == nullptr || !local->EnableUseCo())
        return 0;

    auto tls_processer = detail::Processer::TryGetLocalProcesser();
    if (tls_processer == nullptr)
        return 0;

    auto coroutine = tls_processer->GetCurrentCoroutine();
    if (coroutine == nullptr)
        return 0;

    return coroutine->GetStackSize();
}

}