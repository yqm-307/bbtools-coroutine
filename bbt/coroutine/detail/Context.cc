#include <bbt/coroutine/detail/Context.hpp>

namespace bbt::coroutine::detail
{

Context::Context(size_t stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool stack_protect):
    m_stack(stack_size, stack_protect),
    m_user_main(co_func),
    m_final_handle(co_final_cb)
{
    void* stack = m_stack.StackTop() + m_stack.UseableSize();
    m_context = boost::context::detail::make_fcontext(stack, m_stack.UseableSize(), &Context::_CoroutineMain);
}

Context::~Context()
{
}



}