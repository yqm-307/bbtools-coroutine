#include <bbt/coroutine/detail/Defer.hpp>

namespace bbt::coroutine::detail {

Defer::Defer(const DeferCallback& cb) : m_defer_handler(cb)
{
}

Defer::~Defer()
{
    /* 空回调直接跳过；异常不做隔离，调用方按头文件约束保证不抛。 */
    if (m_defer_handler)
        m_defer_handler();
}

}