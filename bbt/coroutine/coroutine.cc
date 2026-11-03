#include <bbt/coroutine/coroutine.hpp>
#include <stdexcept>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>

namespace bbt::coroutine
{

detail::AwaitHelper::AwaitHelper()
{
    if (GetLocalCoroutineId() == 0)
        throw std::logic_error("bbt_co_await requires a coroutine context");
}

bool IsInCoroutineContext() {
    return GetLocalCoroutineId() != 0;
}

void RequireCoroutineContext() {
    if (!IsInCoroutineContext())
        throw std::logic_error("operation requires a coroutine context");
}

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