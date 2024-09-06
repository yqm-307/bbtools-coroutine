#pragma once
#include <bbt/coroutine/detail/Defer.hpp>

namespace bbt::coroutine
{

class _DeferHelper
{
public:
    
    detail::Defer operator-(detail::DeferCallback&& co_func)
    {
        // detail::Defer tmp_obj
        return std::move(detail::Defer{std::forward<detail::DeferCallback&&>(co_func)});
    }

};

}