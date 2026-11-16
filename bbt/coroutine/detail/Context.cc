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

namespace bbt::coroutine::detail {

#ifdef BBT_COROUTINE_STACK_TESTING
namespace {
std::atomic_bool s_failContext{false};
}
void Context::_TestFailNextCreate() noexcept
{
    s_failContext.store(true);
}
#endif

fcontext_t& Context::GetCurThreadContext()
{
    static thread_local fcontext_t _context = nullptr;
    return _context;
}

void Context::_CoroutineMain(boost::context::detail::transfer_t transfer)
{

    /**
     * data 是 _Resume 传入的 Context*
     */
    Assert(transfer.data != nullptr);
    auto* context = reinterpret_cast<Context*>(transfer.data);

    /**
     * 保存来源（worker）上下文：_Yield 切回时用它，所以每次切入都要重写本线程 TLS。
     */
    context->GetCurThreadContext() = transfer.fctx;

    /**
     * 用户主函数在本协程栈上执行。协程内任何阻塞调用都必须经 bbt::co::sync 转成挂起，
     * 否则会占死 worker 线程；异常不得逃出 fcontext，统一在这里收口。
     */
    try {
        context->m_user_main();
    } catch (...) {
        const auto eptr = std::current_exception();
        Coroutine* co = g_bbt_tls_coroutine_co;
        if (co != nullptr)
            co->OnException(eptr);

        /**
         * 异常文本提取一份，日志与回调共用
         */
        std::string what{"unknown exception"};
        try {
            if (eptr)
                std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }

        if (g_bbt_coroutine_config->RuntimeSnapshot().coevent_exception_callback != nullptr) {
            try {
                g_bbt_coroutine_config->RuntimeSnapshot().coevent_exception_callback(core::errcode::Errcode(what));
            } catch (...) {

                /**
                 * 回调异常不得逃出 fcontext
                 */
            }
        } else {

            /**
             * 运行时契约：detached 无人接收的异常必须日志 + 计数，禁止静默丢失。
             * re-throw 会逃出 fcontext 导致 terminate，故交付 = 日志+计数。
             * WHY 不用 WarnPrint：本 catch 运行在协程自身栈上，而
             * DebugPrint 族内部各带 char[4096] 栈缓冲（vformat+VPrint≈8KB），
             * 会打爆默认 4KB 协程栈；直接 fprintf 到 stderr，栈占用有界。
             * 日志带 co id：与 parked 现场串联定位是哪个协程。
             */
            const CoroutineId co_id = (co != nullptr) ? co->GetId() : BBT_COROUTINE_INVALID_COROUTINE_ID;
            std::fprintf(stderr, "[bbtco] unhandled exception co=%u: %.200s\n", static_cast<unsigned>(co_id),
                         what.c_str());
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

Context::Context(size_t stack_size, const CoroutineCallback& co_func, bool stack_protect)
    : Context(g_bbt_stackpoll->Acquire({stack_size, stack_protect}), co_func)
{
}

Context::Context(StackLease stack, const CoroutineCallback& co_func) : m_user_main(co_func), m_stack(std::move(stack))
{
    if (!m_user_main || !m_stack)
        throw std::runtime_error("coroutine context requires a callback and stack lease");
#ifdef BBT_COROUTINE_STACK_TESTING
    if (!s_failContext.exchange(false))
#endif
        m_context = boost::context::detail::make_fcontext(m_stack->StackTop(), m_stack->UseableSize(),
                                                          &Context::_CoroutineMain);
    if (!m_context)
        throw std::runtime_error("make_fcontext failed");

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS

    /**
     * make_fcontext 成功后、任何 jump 之前初始化采样边界：bottom/top 取真实
     * StackBottom/StackTop，min_sp 从 top 起计（尚无样本时 observed = 0）。
     * 复用栈池中的旧栈不继承上一任协程的历史样本。
     */
    m_stackBudgetState.bottom = reinterpret_cast<uintptr_t>(m_stack->StackBottom());
    m_stackBudgetState.top = reinterpret_cast<uintptr_t>(m_stack->StackTop());
    m_stackBudgetState.min_sp = m_stackBudgetState.top;
    m_stackBudgetState.sample_count = 0;
#endif
}

Context::~Context()
{

    /**
     * 先让两个 std::function 释放 target，再归还栈。callable 的捕获对象（含本
     * 上下文最后持有的业务资源）在 ~Context 函数体内同步析构，必须先于
     * Release(m_stack)：若先归还栈，栈即回到栈池可被复用、进程也可能在该窗口
     * 退出，此时仍挂在本对象上的捕获对象才析构，生命周期就越过了“栈已归还”
     * 这条观测界线。置空顺序与成员逆序析构一致（m_onyield_callback 声明在后、
     * 先析构）。
     */
    m_onyield_callback = nullptr;
    m_user_main = nullptr;
    m_stack.reset();
}

void Context::Yield()
{
    _Yield();
}

int Context::YieldWithCallback(const CoroutineOnYieldCallback& cb)
{

    /**
     * CoEvent 的触发来自别的线程（poller 派发或其他 worker 的 Notify）。若在挂起前就
     * 注册事件，回调可能在协程还没让出时把它唤醒——一个尚未挂起的协程被 Resume 会把
     * 状态机打乱。所以注册动作放在 on-yield 回调里，由 Resume 在真正让出之后执行。
     */
    int ret = 0;

    Assert(m_onyield_callback == nullptr);
    m_onyield_callback_result = YieldCheckStatus::NO_CHECK;
    try {
        m_onyield_callback = cb;
    } catch (...) {

        /**
         * target 非 SBO 时存储复制可抛 bad_alloc。此时协程
         * 尚未让出，回调槽按「未登记」收口——恢复空槽与 NO_CHECK，由
         * Coroutine::YieldWithCallback 负责 await/运行态回滚。本 catch
         * 只做成员复位，不产生新分配。
         */
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

    /**
     * 执行 on-yield 检查。check 失败的重入会继续跑协程，其间可能再次挂出新回调，必须循环到没有
     * 待执行回调：只重入一次的话新回调没有执行者，协程带着未注册的 await 事件停在 EVENT_WAIT，
     * CommitPark 失败后被 Processer 丢弃引用，永久悬挂且无回收点。协程侧在 _Yield 返回后清空
     * m_onyield_callback，本循环每次读到的是最新挂出的回调。
     */
    while (m_onyield_callback) {
        const bool check_succ = m_onyield_callback();
        m_onyield_callback_result =
            bbt_likely(check_succ) ? YieldCheckStatus::CHECK_SUCCESS : YieldCheckStatus::CHECK_FAILED;
        if (bbt_likely(check_succ))
            break;

        /**
         * check失败就回到原本协程通知一下check失败了
         */
        _Resume();
    }
}

int Context::_Yield()
{

    /**
     * jump 回 TLS 记录的来源上下文。jump 返回说明来源线程又 Resume 了本协程，
     * transfer.fctx 是本次的来源，写回 TLS 供下一次 _Yield 切回。
     */
    boost::context::detail::transfer_t transfer{fctx : nullptr, data : nullptr};

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS

    /**
     * 让出前先清零归属：jump 之后本线程跑的是 worker/调度逻辑（含
     * on-yield 回调），不属于本协程，不该计入它的样本。
     * 不把旧指针保存下来在 jump 返回后恢复——本协程再次被 Resume 时
     * （可能在另一个 worker 线程），是由那次 _Resume 负责重新挂接同一
     * Context 状态；这里恢复反而会覆盖新协程已挂好的归属。
     */
    ExchangeActiveStackBudgetState(nullptr);
#endif

    transfer = boost::context::detail::jump_fcontext(GetCurThreadContext(), &m_context);

    GetCurThreadContext() = transfer.fctx;

    return 0;
}

int Context::_Resume()
{

    /**
     * 切入本协程；jump 返回即协程又让出了。transfer.data 是上面传进去的 this，
     * 协程侧把它当 void** 回写自己的返回 fctx（m_context 在偏移 0）。
     */
    boost::context::detail::transfer_t transfer{fctx : nullptr, data : nullptr};

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS

    /**
     * jump 前把本线程 TLS 挂到本协程状态：之后在协程栈上发生的插桩入口
     * 都计入本 Context。previous 只保留在本函数局部，jump 返回后恢复调用
     * 方此前的诊断归属（通常为 nullptr）——这只是不破坏调用者归属的归还，
     * 不构成对任意嵌套 Context Resume 的支持承诺（既有
     * GetCurThreadContext 线程 TLS 的嵌套语义不在本诊断范围内）。
     * TLS 读写都经 noinline 交换函数，防止编译器跨 jump_fcontext 缓存
     * 旧 TLS 地址。
     */
    StackBudgetState* prev_state = ExchangeActiveStackBudgetState(&m_stackBudgetState);
#endif

    transfer = boost::context::detail::jump_fcontext(m_context, reinterpret_cast<void*>(this));

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS

    /**
     * jump 返回 = 协程已让出（其 _Yield 已清零）或到终态；恢复外层归属，
     * 本线程不留本协程残留。
     */
    ExchangeActiveStackBudgetState(prev_state);
#endif

    /**
     * 保存来源协程，因为可能因为yield让出cpu，没有执行完
     */
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

    /**
     * 前置条件见 Context.hpp：owner 协程自身同步安全点，或调用方已同步
     * 确认协程不在运行；无同步并发读禁止。
     * observed_sample_bytes = top - min_sp，是插桩入口观测到的最深栈延伸，
     * coverage 恒 PARTIAL；没有样本返回全 0（不伪造水位）。
     */
    StackBudgetSample sample{};
    if (m_stackBudgetState.sample_count == 0)
        return sample;

    sample.observed_sample_bytes = static_cast<size_t>(m_stackBudgetState.top - m_stackBudgetState.min_sp);
    sample.sample_count = m_stackBudgetState.sample_count;
    return sample;
}
#endif

}