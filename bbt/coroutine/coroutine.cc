#include <bbt/coroutine/coroutine.hpp>

namespace bbt::coroutine
{

detail::CoroutineId GetLocalCoroutineId()
{
    auto coroutine = g_bbt_coroutine_co;
    return coroutine->GetId();
}

}