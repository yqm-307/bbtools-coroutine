#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>

namespace bbt::coroutine
{

detail::CoroutineId GetLocalCoroutineId()
{
    auto coroutine = g_bbt_tls_coroutine_co;
    if (coroutine == nullptr)
        return 0;
    return coroutine->GetId();
}

}