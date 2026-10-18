#pragma once
#include <cstddef>
#include <cstdint>

/**
 * @file StackBudgetDiagnostics.hpp
 * @brief 可选协程栈采样诊断（B4）。仅在 BBT_COROUTINE_STACK_DIAGNOSTICS
 * 构建（Linux x86_64 + GNU，-finstrument-functions）存在；OFF 构建不包含
 * 任何状态、API 或 hook 引用。
 *
 * 语义边界（固定，不放宽）：
 *  - 采样的是「插桩函数入口处的硬件 SP」，只在函数入口打点；mid-function
 *    alloca/VLA、未插桩 TU（第三方/旧对象）、信号路径产生的栈延伸都观测不到，
 *    所以 coverage 恒为 PARTIAL，样本不是栈用量的上界或下界，更不能当
 *    生产容量证明。
 *  - 无全栈填色、无保护页修改、无诊断期堆分配；hook 只做整数/指针比较，
 *    不分配、不日志、不抛异常、不取锁、不调用 runtime 查询。
 *  - hook 自身开销包含在样本里：打点发生在函数入口，样本反映的是
 *    「含插桩成本」的布局，不能直接外推生产栈深。
 */

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS

namespace bbt::coroutine::detail
{

/**
 * @brief 快照覆盖度。当前唯一值 PARTIAL：观测面不完整（未插桩路径、
 * mid-function alloca 等不可见），不存在 FULL。
 */
enum class StackBudgetCoverage
{
    PARTIAL = 0,
};

/**
 * @brief 单协程采样状态（POD，32B）。生命周期 = Context 持栈期，
 * 随 Context 构造初始化、随栈归还销毁；复用池中栈不继承历史样本。
 *
 * 并发约定：state 由当前 owner 线程的 hook 同步写（协程运行中）；
 * 读取只允许在同步安全点——见 Context::GetStackBudgetSample 前置条件。
 */
struct StackBudgetState
{
    uintptr_t   bottom{0};          ///< 可用栈低边界（StackBottom）
    uintptr_t   top{0};             ///< 栈顶（初始 SP 侧，StackTop）
    uintptr_t   min_sp{0};          ///< 观测到的最低 SP（最深水位），初始 = top
    uint64_t    sample_count{0};    ///< 有效样本数，饱和计数不回绕
};

/**
 * @brief 安全点导出的快照（POD）。
 *
 * observed_sample_bytes = top - min_sp，是「被插桩路径上观测到的最深
 * 栈延伸字节数」；无样本时为 0 且 sample_count = 0。
 */
struct StackBudgetSample
{
    size_t                  observed_sample_bytes{0};
    uint64_t                sample_count{0};
    StackBudgetCoverage     coverage{StackBudgetCoverage::PARTIAL};
};

/**
 * @brief 替换当前线程的活动采样归属，返回被替换的指针。
 * （本线程内的普通读写，不是跨线程原子 API。）
 *
 * TLS 只保存「当前线程正在运行的协程」的状态指针，不存历史：
 * _Resume 挂接本协程状态并暂存外层值，协程让出后恢复外层；_Yield
 * 在让出前清零——on-yield 回调跑在 worker 栈上时不属于任何协程。
 * 协程跨 fcontext 迁移后由新 owner 线程的 _Resume 重新挂接同一
 * Context 状态，旧线程不留残留。
 *
 * noinline + no_instrument_function：强制在调用点重新解析 TLS 地址，
 * 避免编译器跨 jump_fcontext 复用旧 TLS 地址；且自身不可被插桩
 * （hook 内重入会自递归）。
 */
StackBudgetState*   ExchangeActiveStackBudgetState(StackBudgetState* next) noexcept;

/**
 * @brief 读当前线程活动状态（诊断/测试自检用，断言线程无残留归属）。
 */
StackBudgetState*   GetActiveStackBudgetState() noexcept;

}

#endif // BBT_COROUTINE_STACK_DIAGNOSTICS
