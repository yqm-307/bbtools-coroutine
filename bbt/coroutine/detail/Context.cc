#include <exception>
#include <memory>
#include <string>

#include <bbt/coroutine/detail/Context.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

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
    try {
        context->m_user_main();
    }
    catch (...)
    {
        const auto eptr = std::current_exception();
        Coroutine* co = g_bbt_tls_coroutine_co;
        if (co != nullptr)
            co->OnException(eptr);

        /* 异常文本提取一份，日志与回调共用 */
        std::string what{"unknown exception"};
        try {
            if (eptr)
                std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }

        if (g_bbt_coroutine_config->m_ext_coevent_exception_callback != nullptr) {
            try {
                g_bbt_coroutine_config->m_ext_coevent_exception_callback(core::errcode::Errcode(what));
            } catch (...) {
                /* 回调异常不得逃出 fcontext */
            }
        } else {
            /* 运行时契约：detached 无人接收的异常必须日志 + 计数，禁止静默丢失。
             * re-throw 会逃出 fcontext 导致 terminate，故交付 = 日志+计数。
             * WHY 不用 WarnPrint：本 catch 运行在协程自身栈上，而
             * DebugPrint 族内部各带 char[4096] 栈缓冲（vformat+VPrint≈8KB），
             * 会打爆默认 4KB 协程栈；直接 fprintf 到 stderr，栈占用有界。
             * 日志带 co id：与 parked 现场串联定位是哪个协程。 */
            const CoroutineId co_id = (co != nullptr) ? co->GetId() : BBT_COROUTINE_INVALID_COROUTINE_ID;
            std::fprintf(stderr, "[bbtco] unhandled exception co=%u: %.200s\n",
                         static_cast<unsigned>(co_id), what.c_str());
            g_bbt_coroutine_config->m_unhandled_exception_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

#if defined(BBT_COROUTINE_PROFILE)
    g_bbt_profiler->OnEvent_DoneCoroutine();
#endif

    /**
     * 执行完后，切回到调度逻辑中去
     */
    context->Yield();
}


Context::Context(size_t stack_size, const CoroutineCallback& co_func, bool stack_protect):
    m_user_main(co_func)
{
    Assert(m_user_main != nullptr);

    /* callable 先构造（初始化列表）再借栈；显式大小与保护属性真正传给栈池，
     * 不再被全局默认覆盖。
     * 借栈到 make_fcontext 成功期间用窄作用域 RAII（复用 std::unique_ptr 加
     * 自定义归还 deleter）暂持归还责任：容量不足或 make_fcontext 失败都必须
     * 先归还再抛出；成功后才把裸指针交给本对象作为唯一借用责任。 */
    auto stack_returner = [](Stack* stack) {
        if (stack != nullptr)
            g_bbt_stackpoll->Release(stack);
    };
    std::unique_ptr<Stack, decltype(stack_returner)> stack_guard(
        g_bbt_stackpoll->Apply(stack_size, stack_protect), stack_returner);

    if (stack_guard == nullptr) {
        throw std::runtime_error("Coroutine stack not enough! Please try adjust the globalconfig 'm_cfg_stackpool_max_alloc_size'");
    }

    void* stack = stack_guard->StackTop();
    m_context = boost::context::detail::make_fcontext(stack, stack_guard->UseableSize(), &Context::_CoroutineMain);
    m_stack = stack_guard.release();

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS
    /* make_fcontext 成功后、任何 jump 之前初始化采样边界：bottom/top 取真实
     * StackBottom/StackTop，min_sp 从 top 起计（尚无样本时 observed = 0）。
     * 复用栈池中的旧栈不继承上一任协程的历史样本。 */
    m_stackBudgetState.bottom       = reinterpret_cast<uintptr_t>(m_stack->StackBottom());
    m_stackBudgetState.top          = reinterpret_cast<uintptr_t>(m_stack->StackTop());
    m_stackBudgetState.min_sp       = m_stackBudgetState.top;
    m_stackBudgetState.sample_count = 0;
#endif
}

Context::~Context()
{
    /* 先让两个 std::function 释放 target，再归还栈。callable 的捕获对象（含本
     * 上下文最后持有的业务资源）在 ~Context 函数体内同步析构，必须先于
     * Release(m_stack)：若先归还栈，栈即回到栈池可被复用、进程也可能在该窗口
     * 退出，此时仍挂在本对象上的捕获对象才析构，生命周期就越过了“栈已归还”
     * 这条观测界线。置空顺序与成员逆序析构一致（m_onyield_callback 声明在后、
     * 先析构），不改变对象布局。 */
    m_onyield_callback = nullptr;
    m_user_main = nullptr;
    if (m_stack != nullptr) {
        g_bbt_stackpoll->Release(m_stack);
        m_stack = nullptr;
    }
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
    try {
        m_onyield_callback = cb;
    } catch (...) {
        /* target 非 SBO 时存储复制可抛 bad_alloc。此时协程
         * 尚未让出，回调槽按「未登记」收口——恢复空槽与 NO_CHECK，由
         * Coroutine::YieldWithCallback 负责 await/运行态回滚。本 catch
         * 只做成员复位，不产生新分配。 */
        m_onyield_callback = nullptr;
        m_onyield_callback_result = YieldCheckStatus::NO_CHECK;
        throw;
    }

    _Yield();

    if (bbt_unlikely(m_onyield_callback_result == YieldCheckStatus::CHECK_FAILED))
        ret = -1;

    m_onyield_callback = nullptr;
    m_onyield_callback_result = YieldCheckStatus::NO_CHECK;

    return ret;
}


void Context::Resume()
{
    _Resume();

    /* 执行on yield success。check 失败的重入会继续执行协程，其间协程
     * 可能再次 YieldWithCallback 挂出新的 onyield 回调——必须循环处理
     * 直到没有待执行回调。只重入一次的旧实现会丢掉新回调：协程带着未
     * 注册的 await 事件停在 EVENT_WAIT，CommitYield 的 CommitPark 失败
     * 后被 Processer 丢弃引用，协程永久悬挂且无回收点。
     * 注：协程侧 YieldWithCallback 在 _Yield 返回后自行清空
     * m_onyield_callback，本循环每次读到的是协程最新挂出的回调。 */
    while (m_onyield_callback) {
        const bool check_succ = m_onyield_callback();
        m_onyield_callback_result =
            bbt_likely(check_succ) ? YieldCheckStatus::CHECK_SUCCESS
                                 : YieldCheckStatus::CHECK_FAILED;
        if (bbt_likely(check_succ))
            break;
        /* check失败就回到原本协程通知一下check失败了 */
        _Resume();
    }
}


int Context::_Yield()
{
    /**
     * 调用jump后，切换回调度线程
     *
     * 当jump返回时，说明调度线程通过 Resume 返回了。trf中保存了调度协程的上下文
     */
    boost::context::detail::transfer_t transfer{fctx: nullptr, data: nullptr};

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS
    /* 让出前先清零归属：jump 之后本线程跑的是 worker/调度逻辑（含
     * on-yield 回调），不属于本协程，不该计入它的样本。
     * 不把旧指针保存下来在 jump 返回后恢复——本协程再次被 Resume 时
     * （可能在另一个 worker 线程），是由那次 _Resume 负责重新挂接同一
     * Context 状态；这里恢复反而会覆盖新协程已挂好的归属。 */
    ExchangeActiveStackBudgetState(nullptr);
#endif

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

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS
    /* jump 前把本线程 TLS 挂到本协程状态：之后在协程栈上发生的插桩入口
     * 都计入本 Context。previous 只保留在本函数局部，jump 返回后恢复调用
     * 方此前的诊断归属（通常为 nullptr）——这只是不破坏调用者归属的归还，
     * 不构成对任意嵌套 Context Resume 的支持承诺（既有
     * GetCurThreadContext 线程 TLS 的嵌套语义不在本诊断范围内）。
     * TLS 读写都经 noinline 交换函数，防止编译器跨 jump_fcontext 缓存
     * 旧 TLS 地址。 */
    StackBudgetState* prev_state = ExchangeActiveStackBudgetState(&m_stackBudgetState);
#endif

    transfer = boost::context::detail::jump_fcontext(m_context, reinterpret_cast<void*>(this));

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS
    /* jump 返回 = 协程已让出（其 _Yield 已清零）或到终态；恢复外层归属，
     * 本线程不留本协程残留。 */
    ExchangeActiveStackBudgetState(prev_state);
#endif


    // 保存来源协程，因为可能因为yield让出cpu，没有执行完
    auto context = transfer.data;
    *(void**)context = transfer.fctx;

    return 0;
}

size_t Context::GetStackSize() const noexcept
{
    return m_stack->UseableSize();
}

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS
StackBudgetSample Context::GetStackBudgetSample() const noexcept
{
    /* 前置条件见 Context.hpp：owner 协程自身同步安全点，或调用方已同步
     * 确认协程不在运行；无同步并发读禁止。
     * observed_sample_bytes = top - min_sp，是插桩入口观测到的最深栈延伸，
     * coverage 恒 PARTIAL；没有样本返回全 0（不伪造水位）。 */
    StackBudgetSample sample{};
    if (m_stackBudgetState.sample_count == 0)
        return sample;

    sample.observed_sample_bytes =
        static_cast<size_t>(m_stackBudgetState.top - m_stackBudgetState.min_sp);
    sample.sample_count = m_stackBudgetState.sample_count;
    return sample;
}
#endif


}