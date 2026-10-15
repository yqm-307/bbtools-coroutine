#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>

namespace bbt::coroutine
{

detail::CoroutineId GetLocalCoroutineId()
{
    if (!detail::LocalThread::GetTLSInst()->EnableUseCo())
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
    if (!detail::LocalThread::GetTLSInst()->EnableUseCo())
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