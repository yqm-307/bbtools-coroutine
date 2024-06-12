#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine::detail
{

Coroutine::Coroutine(int stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool need_protect):
    m_context(stack_size, co_func, co_final_cb, need_protect)
{
}

Coroutine::~Coroutine()
{

}

void Coroutine::Resume()
{
    m_context.Resume();
}

void Coroutine::Yield()
{
    m_context.Yield();
}

}