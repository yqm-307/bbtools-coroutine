#pragma once
#include <functional>
#include <boost/context/detail/fcontext.hpp>
#include <bbt/base/assert/Assert.hpp>
#include <bbt/coroutine/detail/Stack.hpp>

namespace bbt::coroutine::detail
{

using fcontext_t = boost::context::detail::fcontext_t;

class Context
{
public:
    Context(size_t stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final, bool stack_protect = true);
    ~Context();

    // 挂起当前协程，回到上一次记录的协程
    void Yield();

    /* 唤醒当前协程 */
    void Resume();

    static fcontext_t& GetCurThreadContext();

protected:
    static void _CoroutineMain(boost::context::detail::transfer_t transfer);
private:
    fcontext_t              m_context;
    CoroutineCallback       m_user_main{nullptr};
    CoroutineFinalCallback  m_final_handle{nullptr};
    Stack*                  m_stack{nullptr};                    

};

}