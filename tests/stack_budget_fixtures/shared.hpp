#pragma once

#ifndef STACK_BUDGET_ENTRY_FACTOR
#define STACK_BUDGET_ENTRY_FACTOR 3
#endif

int CrossCallee(int);
extern "C" int ThirdPartyOpaque(int);

struct StackBudgetOperation
{
    virtual int Run(int) = 0;
};

struct StackBudgetConcrete : StackBudgetOperation
{
    int Run(int value) override { return value + 17; }
};
