#include <bbt/coroutine/detail/Profiler.hpp>

namespace bbt::coroutine::detail
{

Profiler::UPtr& Profiler::GetInstance()
{
    static UPtr _inst = nullptr;
    if (_inst != nullptr)
        _inst = UPtr(new Profiler());

    return _inst;
}


} // namespace bbt::coroutine::detail
