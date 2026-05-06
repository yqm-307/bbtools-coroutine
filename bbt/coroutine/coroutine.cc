#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/Trace.hpp>

namespace bbt::coroutine
{

detail::CoroutineId GetLocalCoroutineId()
{
    auto tls_processer = g_bbt_tls_processer;
    if (tls_processer == nullptr)
        return 0;

    auto coroutine = g_bbt_tls_coroutine_co;
    if (coroutine == nullptr)
        return 0;

    return coroutine->GetId();
}

detail::ProcesserId GetLocalProcesserId()
{
    auto tls_processer = g_bbt_tls_processer;
    if (tls_processer == nullptr)
        return 0;

    return tls_processer->GetId();
}

size_t GetLocalCoroutineStackSize()
{
    auto tls_processer = g_bbt_tls_processer;
    if (tls_processer == nullptr)
        return 0;

    auto coroutine = g_bbt_tls_coroutine_co;
    if (coroutine == nullptr)
        return 0;

    return coroutine->GetStackSize();
}

void SetCoroutineTraceFilter(const CoroutineTraceFilter& filter)
{
    g_bbt_trace->SetFilter(filter);
}

void ResetCoroutineTraceFilter()
{
    g_bbt_trace->ResetFilters();
}

CoroutineTraceSnapshot QueryCoroutineTrace(detail::CoroutineId id)
{
    return g_bbt_trace->QueryCoroutine(id);
}

std::vector<CoroutineTraceMeta> QueryActiveCoroutinesByDesc(const std::string& desc, bool prefix)
{
    return g_bbt_trace->QueryActiveCoroutinesByDesc(desc, prefix);
}

std::string DumpCoroutineTrace(detail::CoroutineId id)
{
    return g_bbt_trace->DumpCoroutine(id);
}


} 
