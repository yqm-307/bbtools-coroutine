/* 协程上下文 Context：借一条栈、持一个 fcontext，负责一次「切进去 / 切回来」。 */
#pragma once
#include <functional>
#include <boost/context/detail/fcontext.hpp>
#include <bbt/core/util/Assert.hpp>
#include <bbt/coroutine/detail/Stack.hpp>
#include <bbt/coroutine/detail/debug/StackBudgetDiagnostics.hpp>

namespace bbt::coroutine::detail {

using fcontext_t = boost::context::detail::fcontext_t;

/* on-yield 判定结果：CHECK_* 由 Resume 在回调返回后写入，YieldWithCallback 据此返回 0/-1。 */
enum YieldCheckStatus {
    NO_CHECK = 0,
    CHECK_SUCCESS = 1,
    CHECK_FAILED = 2,
};

/** @brief 协程上下文：StackPool 借出的栈 + fcontext，负责一次切进/切回；栈由本对象独占直到析构归还。
 * Yield 只能在本协程栈上调用；Resume 可由任意 worker 发起，「从哪切进来」记在 TLS，每次 _Yield 返回时重写。 */
class Context {
public:
    /**
     * @brief 借栈并建立协程入口。@p stack_size 是请求字节数，页对齐后才是可用容量；@p co_func 为 nullptr 触发 Assert。
     * @throws std::runtime_error 栈池已达 m_cfg_stackpool_max_alloc_size，借不到栈。
     */
    Context(size_t stack_size, const CoroutineCallback& co_func, bool stack_protect = true);
    /** @brief 先析构两个 std::function 的 target 再归还栈：捕获对象可能持有本上下文最后的业务资源，顺序不可换。 */
    ~Context();

    /** @brief 让出，切回 GetCurThreadContext 记录的来源上下文；只能在本协程栈上调用，返回即已被 Resume 重新切入。 */
    void Yield();

    /** @brief 先挂回调再让出，让事件注册发生在协程真正挂起之后；回调在 Resume 侧执行（不在协程栈上）。
     * 返回 0 = 成功挂起并被唤醒，-1 = 回调判定失败（未真正挂起）。回调存储失败（bad_alloc）会在未让出时抛出，
     * 槽位按未登记复位，由调用方回滚 await/运行态。 */
    int YieldWithCallback(const CoroutineOnYieldCallback& cb);

    /**
     * @brief 切入本协程；返回时协程已让出或到终态。
     * 返回前把挂出的 on-yield 回调逐个判定完，失败就再切回去，循环到没有待执行回调。
     */
    void Resume();

    /** @brief 本线程「从哪切进来」的 fcontext：协程侧是最近一次 Resume 方，worker 侧是调度循环自身。 */
    static fcontext_t& GetCurThreadContext();

    /** @brief 栈的可用字节数（页对齐后，不含保护页），可直接给 make_fcontext。 */
    size_t GetStackSize() const noexcept;

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS
    /**
     * @brief 导出栈采样快照。
     *
     * 前置条件（两种合法调用，满足其一）：
     *  - 当前正在执行的唯一 owner 协程在自身同步安全点调用——hook 与调用
     *    同线程同步发生，不存在并发写；
     *  - 非 owner 调用方已同步确认该协程不在运行（Resume 已返回/协程
     *    已让出或到终态）。
     * 其它情形（无同步的并发读）禁止：会读到撕裂值且无意义。
     * 本函数不加锁、不做快照校验。返回值是 PARTIAL 观测样本，不是栈用量
     * 上下界。
     */
    StackBudgetSample GetStackBudgetSample() const noexcept;
#endif

protected:
    /* 协程入口：transfer.data 是 Context*，transfer.fctx 是来源（worker）上下文。
     * 入口把来源 fctx 存进 TLS 供 _Yield 切回，执行用户主函数，最后 Yield 回调度线程。 */
    static void _CoroutineMain(boost::context::detail::transfer_t transfer);

    // 让出/切入的实际 jump；_Yield 返回时用 transfer.fctx 重写本线程的来源上下文。
    int _Yield();
    int _Resume();

private:
    /* 必须是第一个成员：_Resume 把 Context* 直接当 void** 回写协程侧的返回 fctx。 */
    fcontext_t m_context{nullptr};
    CoroutineCallback m_user_main{nullptr}; // 用户主函数；~Context 先释放它再归还栈
    Stack* m_stack{nullptr};                // 从 StackPool 借出的栈，~Context 归还；非空即本对象独占

    CoroutineOnYieldCallback m_onyield_callback{nullptr};
    YieldCheckStatus m_onyield_callback_result{YieldCheckStatus::NO_CHECK};

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS
    /* 采样状态随 Context（= 栈借用期）存在；TLS 只存指向它的活动指针。 */
    StackBudgetState m_stackBudgetState{};
#endif
};

}