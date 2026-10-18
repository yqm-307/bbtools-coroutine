#include "entry.hpp"
#include <bbt/coroutine/coroutine.hpp>

std::size_t stack_budget_capacity_seen{0};
int stack_budget_result{0};
int stack_budget_done{0};

extern "C" __attribute__((noinline)) int StackBudgetLeaf(int value)
{
    volatile char frame[192];
    frame[value & 127] = static_cast<char>(value);
    return frame[value & 127] + 7;
}

extern "C" void StackBudgetEntry()
{
    volatile char frame[128];
    frame[0] = 3;
    const int result = StackBudgetLeaf(frame[0]);
    const auto capacity = bbt::coroutine::GetLocalCoroutineStackSize();
    __atomic_store_n(&stack_budget_result, result, __ATOMIC_RELAXED);
    __atomic_store_n(&stack_budget_capacity_seen, capacity, __ATOMIC_RELAXED);
    __atomic_store_n(&stack_budget_done, 1, __ATOMIC_RELEASE);
}
