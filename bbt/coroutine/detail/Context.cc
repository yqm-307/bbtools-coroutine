#include <bbt/coroutine/detail/Context.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>

namespace bbt::coroutine::detail
{

fcontext_t& Context::GetCurThreadContext()
{
    static thread_local fcontext_t _context = nullptr;
    return _context;
}

void Context::_CoroutineMain(boost::context::detail::transfer_t transfer)
{
    Assert(transfer.data != nullptr); // 来源协程上下文
    auto* context = reinterpret_cast<Context*>(transfer.data);

    context->GetCurThreadContext() = transfer.fctx;

    context->m_user_main();

    if (context->m_final_handle != nullptr)
        context->m_final_handle();
    
    context->Yield();
}


Context::Context(size_t stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool stack_protect):
    m_stack(g_bbt_stackpoll->Apply()),
    m_user_main(co_func),
    m_final_handle(co_final_cb)
{
    Assert(m_user_main != nullptr);
    if (m_stack == nullptr) {
        throw std::runtime_error("Coroutine stack not enough! Please try adjust the globalconfig 'm_cfg_stackpool_max_alloc_size'");
    }
    void* stack = m_stack->StackTop();
    m_context = boost::context::detail::make_fcontext(stack, m_stack->UseableSize(), &Context::_CoroutineMain);
}

Context::~Context()
{
    g_bbt_stackpoll->Release(m_stack);
}

void Context::Yield()
{
    _Yield();
}

int Context::YieldWithCallback(const CoroutineOnYieldCallback& cb)
{
    int ret = 0;

    Assert(m_onyield_callback == nullptr);
    m_onyield_callback_result = YieldCheckStatus::NO_CHECK;
    m_onyield_callback = cb;

    _Yield();

    ret = m_onyield_callback_result == YieldCheckStatus::CHECK_FAILED ? -1 : ret;

    m_onyield_callback = nullptr;
    m_onyield_callback_result = YieldCheckStatus::NO_CHECK;

    return ret;
}


void Context::Resume()
{
    bool check_succ = true;

    _Resume();

    // 执行on yield success，然后清除掉
    if (m_onyield_callback) {
        check_succ = m_onyield_callback();
        m_onyield_callback_result = check_succ ? YieldCheckStatus::CHECK_SUCCESS : YieldCheckStatus::CHECK_FAILED;
    }

    // check失败就回到原本协程通知一下check失败了
    if (!check_succ)
        _Resume();
}


int Context::_Yield()
{
    /**
     * 调用jump后，切换回调度线程
     * 
     * 当jump返回时，说明调度线程通过 Resume 返回了。trf中保存了调度协程的上下文
     */
    boost::context::detail::transfer_t transfer{fctx: nullptr, data: nullptr};
    transfer = boost::context::detail::jump_fcontext(GetCurThreadContext(), &m_context);

    GetCurThreadContext() = transfer.fctx;

    return 0;
}

int Context::_Resume()
{
    /**
     * 调用jump后，将切换到当前协程
     */
    boost::context::detail::transfer_t transfer{fctx: nullptr, data: nullptr};

    transfer = boost::context::detail::jump_fcontext(m_context, reinterpret_cast<void*>(this));

    // 保存来源协程，因为可能因为yield让出cpu，没有执行完
    auto context = transfer.data;
    *(void**)context = transfer.fctx;

    return 0;
}

}