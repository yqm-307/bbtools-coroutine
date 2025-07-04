#include <bbt/coroutine/detail/Defer.hpp>

namespace bbt::coroutine::detail
{

Defer::Defer(const DeferCallback& cb):
    m_defer_handler(cb)
{
}

Defer::~Defer()
{
    if (m_defer_handler)
        m_defer_handler();
}

}