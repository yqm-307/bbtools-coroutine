#include <functional>
#include <boost/context/detail/fcontext.hpp>
#include <bbt/base/assert/Assert.hpp>
#include <bbt/coroutine/detail/Stack.hpp>

namespace bbt::coroutine::detail
{

using fcontext_t = boost::context::detail::fcontext_t;
typedef std::function<void()> CoroutineCallback;
typedef std::function<void()> CoroutineFinalCallback;

class Context
{
public:
    Context(size_t stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final, bool stack_protect = true);
    ~Context();

    // 挂起当前协程，回到上一次记录的协程
    void Yield()
    {
        /**
         * 调用jump后，切换回调度线程
         * 
         * 当jump返回时，说明调度线程通过 Resume 返回了。trf中保存了调度协程的上下文
         */
        boost::context::detail::transfer_t trf;
        trf = boost::context::detail::jump_fcontext(GetCurThreadContext(), &m_context);

        GetCurThreadContext() = trf.fctx;
    }

    /* 唤醒当前协程 */
    void Resume()
    {
        /**
         * 调用jump后，将切换到当前协程
         */
        boost::context::detail::transfer_t transfer;
        transfer = boost::context::detail::jump_fcontext(m_context, reinterpret_cast<void*>(this));

        // 保存来源协程，因为可能因为yield让出cpu，没有执行完
        auto context = transfer.data;
        *(void**)context = transfer.fctx;
    }

    static fcontext_t& GetCurThreadContext()
    {
        static thread_local fcontext_t _context;
        return _context;
    }

protected:
    static void _CoroutineMain(boost::context::detail::transfer_t transfer)
    {
        Assert(transfer.data != nullptr); // 来源协程上下文
        auto* context = reinterpret_cast<Context*>(transfer.data);

        context->GetCurThreadContext() = transfer.fctx;

        context->m_user_main();

        if (context->m_final_handle != nullptr)
            context->m_final_handle();
        
        context->Yield();
    }

private:
    fcontext_t              m_context;
    CoroutineCallback       m_user_main{nullptr};
    CoroutineFinalCallback  m_final_handle{nullptr};
    Stack                   m_stack;

};

}