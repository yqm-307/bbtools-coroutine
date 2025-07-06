#pragma once
#include <bbt/coroutine/detail/Defer.hpp>

namespace bbt::coroutine
{

class _DeferHelper
{
public:
    
    detail::Defer operator-(const detail::DeferCallback& co_func)
    {
        return detail::Defer{co_func};
    }

};

}