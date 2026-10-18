#include <bbt/coroutine/detail/debug/StackBudgetDiagnostics.hpp>

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS

#include <cstdint>

/**
 * @file StackBudgetDiagnostics.cc
 * @brief __cyg_profile_func_enter/exit 采样 hook 与 TLS 交换实现。
 *
 * 本 TU 经 CMake source COMPILE_OPTIONS 固定 -fno-instrument-functions，
 * 函数级再加 no_instrument_function 双保险：hook 及其 helper 都不会产生
 * 插桩调用，否则 __cyg_profile_func_enter 会自递归爆栈。
 */

namespace
{

/**
 * TLS 归属：只指向当前线程正在运行的协程的 StackBudgetState，不存历史。
 * 让出清零、恢复由新 owner 挂接，因此跨 worker 迁移不需要在旧线程清理。
 *
 * tls_model("initial-exec")：默认 global-dynamic TLS 访问会走
 * __tls_get_addr 动态解析（首次访问可能触发分配），hook 在每个插桩函数
 * 入口都要读它，不允许 hook 路径上出现隐式分配。initial-exec 让访问只经
 * 段寄存器 + 静态 TLS 块偏移完成，不调用任何 resolver。
 * 代价：该 TLS 属于静态 TLS 区，要求本库在启动期加载——诊断构建不支持
 * 把 libbbt_coroutine.so 晚期 dlopen（initial-exec 变量在 dlopen 库中会
 * 解析失败）。本诊断仅服务启动期链接的诊断构建，不承诺 dlopen 场景。
 */
thread_local bbt::coroutine::detail::StackBudgetState* t_active_state
    __attribute__((tls_model("initial-exec"))){nullptr};

/**
 * 读真实硬件 SP。必须用 %rsp 而不是局部变量地址：局部变量落在编译器
 * 选定的帧槽位，地址不保证等于当前 SP（红区/帧布局差异）。
 */
__attribute__((noinline, no_instrument_function))
inline uintptr_t ReadHardwareSp() noexcept
{
    uintptr_t sp;
    __asm__ __volatile__("mov %%rsp, %0" : "=r"(sp));
    return sp;
}

}

namespace bbt::coroutine::detail
{

__attribute__((noinline, no_instrument_function))
StackBudgetState* ExchangeActiveStackBudgetState(StackBudgetState* next) noexcept
{
    StackBudgetState* prev = t_active_state;
    t_active_state = next;
    return prev;
}

__attribute__((noinline, no_instrument_function))
StackBudgetState* GetActiveStackBudgetState() noexcept
{
    return t_active_state;
}

}

/**
 * hook 只做基本整数/指针操作：不分配、不日志、不抛异常、不取锁、
 * 不调用 runtime 查询。比较全部手写，不引入 <algorithm> 等 STL 内联
 * 函数——头文件内联函数经符号 interposition 可能绕回本 TU 之外的
 * 插桩实现，在 hook 内造成重入。TLS 访问经 initial-exec 模型（见上），
 * hook 路径上无 __tls_get_addr 调用。
 *
 * 采样语义：函数入口 SP 落在 [bottom, top] 内才记录，min 取最深水位，
 * sample_count 饱和计数（到达 UINT64_MAX 停在原地，不回绕成 0）。
 * 区间外的样本直接丢弃：说明 SP 不属于本协程栈（如调度残留），
 * 不修正、不上报。
 */
extern "C" __attribute__((no_instrument_function))
void __cyg_profile_func_enter(void* /*this_fn*/, void* /*call_site*/) noexcept
{
    using bbt::coroutine::detail::StackBudgetState;

    StackBudgetState* state = t_active_state;
    if (state == nullptr)
        return;

    const uintptr_t sp = ReadHardwareSp();
    if (sp >= state->bottom && sp <= state->top)
    {
        if (sp < state->min_sp)
            state->min_sp = sp;
        if (state->sample_count < UINT64_MAX)
            ++state->sample_count;
    }
}

extern "C" __attribute__((no_instrument_function))
void __cyg_profile_func_exit(void* /*this_fn*/, void* /*call_site*/) noexcept
{
    /* exit 不采样：入口打点已足够形成 min-SP 水位，exit 再加只会放大开销 */
}

#endif // BBT_COROUTINE_STACK_DIAGNOSTICS
