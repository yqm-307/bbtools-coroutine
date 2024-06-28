#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>

namespace bbt::coroutine
{

detail::CoroutineId GetLocalCoroutineId()
{
    auto tls_processer = g_bbt_tls_helper->GetProcesser();
    if (tls_processer == nullptr)
        return 0;

    auto coroutine = g_bbt_tls_coroutine_co;
    if (coroutine == nullptr)
        return 0;

    return coroutine->GetId();
}

}