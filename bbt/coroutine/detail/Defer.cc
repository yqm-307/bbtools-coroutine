#include <bbt/coroutine/detail/Defer.hpp>

namespace bbt::coroutine::detail
{

Defer::Defer(DeferCallback&& cb):
    m_defer_handler(cb)
{
}

Defer::Defer(Defer&& other):
    m_defer_handler(std::move(other.m_defer_handler))
{    
}

Defer::~Defer()
{
    if (m_defer_handler)
        m_defer_handler();
}

}