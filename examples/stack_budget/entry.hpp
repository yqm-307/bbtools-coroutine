#pragma once
#include <cstddef>

extern "C" void StackBudgetEntry();

// Registers the entry coroutine with the runtime using the explicit usable
// capacity U. Defined in registration.cc (the TU that may legitimately pull in
// scheduler symbols); the generated-header glue TU only forwards to it.
void StackBudgetRegister(std::size_t usable_bytes);
extern std::size_t stack_budget_capacity_seen;
extern int stack_budget_result;
extern int stack_budget_done;
