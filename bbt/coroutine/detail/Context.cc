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

    /**
     * 保存来源线程的上下文。为了知道当前协程是从哪个
     * 线程切换过来的在Yield时会用到这个上下文
     */
    context->GetCurThreadContext() = transfer.fctx;

    /**
     * 在这里执行用户协程的主函数。
     * 
     * 用户协程内总是会调用 Yield() 来让出 CPU 控制权，同时
     * 为了防止用户调用某些阻塞操作导致当前线程挂起，我们需要
     * 提供一套非阻塞且使协程挂起的操作（bbt::co::sync提供了
     * 相关类）。
     * 
     */
    context->m_user_main();

#if defined(BBT_COROUTINE_PROFILE)
    g_bbt_profiler->OnEvent_DoneCoroutine();
#endif
    
    /**
     * 执行完后，切回到调度逻辑中去
     */
    context->Yield();
}


Context::Context(size_t stack_size, const CoroutineCallback& co_func, bool stack_protect):
    m_user_main(co_func),
    m_stack(g_bbt_stackpoll->Apply())
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
    /**
     * 因为我们有CoEvent我们需要再协程挂起后再注册事件。
     * 
     * 因为我们的CoEvent触发是在另外的线程执行的，如果挂起前
     * 就注册了CoEvent，那么在尚未挂起，可能会触发CoEvent的回
     * 调，CoEvent会尝试唤醒当前协程，这样就会导致一个协程尚未
     * 被挂起就被唤醒出现异常。
     * 
     */
    int ret = 0;

    Assert(m_onyield_callback == nullptr);
    m_onyield_callback_result = YieldCheckStatus::NO_CHECK;
    m_onyield_callback = cb;

    _Yield();

    if (bbt_unlikely(m_onyield_callback_result == YieldCheckStatus::CHECK_FAILED))
        ret = -1;

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
        if (bbt_likely(check_succ)) {
            m_onyield_callback_result = YieldCheckStatus::CHECK_SUCCESS;
        } else {
            m_onyield_callback_result = YieldCheckStatus::CHECK_FAILED;
        }
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

size_t Context::GetStackSize() const noexcept
{
    return m_stack->UseableSize();
}


}