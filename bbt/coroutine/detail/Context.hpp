#pragma once
#include <functional>
#include <boost/context/detail/fcontext.hpp>
#include <bbt/core/util/Assert.hpp>
#include <bbt/coroutine/detail/Stack.hpp>
#include <bbt/coroutine/detail/debug/StackBudgetDiagnostics.hpp>

namespace bbt::coroutine::detail
{

using fcontext_t = boost::context::detail::fcontext_t;

enum YieldCheckStatus {
    NO_CHECK = 0,
    CHECK_SUCCESS = 1,
    CHECK_FAILED = 2,
};

/**
 * @brief 协程上下文。提供了对栈可切换的上下文操作
 * 
 */
class Context
{
public:
    Context(size_t stack_size, const CoroutineCallback& co_func, bool stack_protect = true);
    ~Context();

    /**
     * @brief 挂起（让出当前CPU控制权）
     */
    void                        Yield();

    /**
     * @brief 挂起并在挂起成功后执行检查函数，如果回调返回false，则返回到调用协程
     * @param cb 
     * @return 0成功，-1表示失败
     */
    int                         YieldWithCallback(const CoroutineOnYieldCallback& cb);

    /**
     * @brief 唤醒（切换到当前协程，给与CPU控制权）
     */
    void                        Resume();

    /**
     * @brief 获取当前线程正在运行的协程上下文
     * @return fcontext_t& 
     */
    static fcontext_t&          GetCurThreadContext();

    /**
     * @brief 获取可用栈总大小
     * 
     * @return size_t 
     */
    size_t                      GetStackSize() const noexcept;

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
    StackBudgetSample           GetStackBudgetSample() const noexcept;
#endif

protected:
    /**
     * @brief 执行协程
     * @param transfer
     */
    static void                 _CoroutineMain(boost::context::detail::transfer_t transfer);

    int                         _Yield();
    int                         _Resume();
private:
    fcontext_t                  m_context{nullptr};
    CoroutineCallback           m_user_main{nullptr};
    Stack*                      m_stack{nullptr};

    CoroutineOnYieldCallback    m_onyield_callback{nullptr};
    YieldCheckStatus            m_onyield_callback_result{YieldCheckStatus::NO_CHECK};

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS
    /* 采样状态随 Context（= 栈借用期）存在；TLS 只存指向它的活动指针。 */
    StackBudgetState            m_stackBudgetState{};
#endif

};

}