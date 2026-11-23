#include "entry.hpp"
#include <bbt/coroutine/detail/Scheduler.hpp>

/**
 * This TU owns the scheduler interaction. RegistCoroutineTask wraps the raw
 * entry pointer in a std::function, so the std::_Function_handler::...::_M_invoke
 * indirect-call target is emitted HERE — not in the generated-header glue TU.
 * That keeps glue.o free of callable frames that would execute on the coroutine
 * stack; the registration object is frozen together with the entry object in the
 * first-stage analysis closure (it is NOT generated-header glue, it is a real
 * input to F).
 */
void StackBudgetRegister(std::size_t usable_bytes)
{
    using namespace bbt::coroutine::detail;
    g_scheduler->Start();
    /**
     * 静态分析需要显式栈大小，见 SyntaxMacro.hpp 的「已知缺口」。
     */
    g_scheduler->RegistCoroutineTask(usable_bytes, &StackBudgetEntry, "conditional-stack-budget-example");
}
