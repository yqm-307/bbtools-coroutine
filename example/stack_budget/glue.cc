#include "entry.hpp"
#include "stack_budget_generated.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main()
{
    // This TU deliberately does NOT touch the scheduler: the RegistCoroutineTask
    // std::function handler (an indirect-call target that runs on the coroutine
    // stack) lives in registration.cc, which is a first-stage analysis input
    // alongside entry.o — not generated-header glue. Here we only forward the
    // bound capacity constant.
    StackBudgetRegister(bbt_stack_budget::ExampleEntry);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (__atomic_load_n(&stack_budget_done, __ATOMIC_ACQUIRE) == 0)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            std::_Exit(2);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto capacity = __atomic_load_n(&stack_budget_capacity_seen, __ATOMIC_RELAXED);
    const int result = __atomic_load_n(&stack_budget_result, __ATOMIC_RELAXED);
    std::printf("conditional user_asserted model: requested U=%zu, "
                "GetLocalCoroutineStackSize=%zu, result=%d\n",
                bbt_stack_budget::ExampleEntry, capacity, result);
    return capacity == bbt_stack_budget::ExampleEntry && result == 10 ? 0 : 1;
}
