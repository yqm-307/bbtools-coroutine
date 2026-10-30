/* bbtco_defer 宏的运算符帮手 _DeferHelper。 */
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