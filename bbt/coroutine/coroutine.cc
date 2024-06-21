#include <bbt/coroutine/coroutine.hpp>

namespace bbt::coroutine
{

detail::CoroutineId GetLocalCoroutineId()
{
    auto coroutine = g_bbt_coroutine_co;
    if (coroutine == nullptr)
        return 0;
    return coroutine->GetId();
}

}