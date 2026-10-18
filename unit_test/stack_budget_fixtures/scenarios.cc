#include "shared.hpp"
#include <cstddef>
#include <functional>

extern "C" __attribute__((noinline)) int DirectLeaf(int value)
{
    volatile char bytes[64];
    bytes[value & 63] = static_cast<char>(value);
    return bytes[value & 63] + 1;
}

extern "C" __attribute__((noinline)) int DirectMiddle(int value)
{
    volatile char bytes[96];
    bytes[value & 63] = static_cast<char>(value);
    return DirectLeaf(value) + bytes[value & 63];
}

extern "C" __attribute__((noinline)) int DirectEntry(int value)
{
    volatile char bytes[32];
    bytes[value & 31] = static_cast<char>(value * STACK_BUDGET_ENTRY_FACTOR);
    return DirectMiddle(value) + bytes[value & 31];
}

extern "C" __attribute__((noinline)) int BranchLeft(int value)
{
    volatile char bytes[128];
    bytes[value & 127] = static_cast<char>(value);
    return bytes[value & 127];
}

extern "C" __attribute__((noinline)) int BranchRight(int value)
{
    volatile char bytes[192];
    bytes[value & 127] = static_cast<char>(value);
    return bytes[value & 127];
}

extern "C" __attribute__((noinline)) int BranchEntry(int value)
{
    const int result = value & 1 ? BranchLeft(value) : BranchRight(value);
    return result + value;
}

extern "C" __attribute__((noinline)) int CrossEntry(int value)
{
    return CrossCallee(value) + value;
}

extern "C" __attribute__((noinline)) int PrebuiltEntry(int value)
{
    return ThirdPartyOpaque(value) + value;
}

extern "C" __attribute__((noinline)) int RecursiveEntry(int value)
{
    volatile int saved = value;
    return value <= 0 ? saved : RecursiveEntry(value - 1) + saved;
}

extern "C" __attribute__((noinline)) int DynamicEntry(std::size_t count)
{
    volatile char* bytes = static_cast<volatile char*>(__builtin_alloca(count + 1));
    bytes[count] = 7;
    asm volatile("" : : "r"(bytes) : "memory");
    return bytes[count];
}

extern "C" __attribute__((noinline)) int VirtualEntry(StackBudgetOperation& operation, int value)
{
    return operation.Run(value) + value;
}

extern "C" __attribute__((noinline)) int RuntimeWrapper(
    const std::function<int(int)>& callback, int value)
{
    return callback(value) + value;
}

extern "C" __attribute__((noinline)) int SwitchEntry(int value)
{
    switch (value & 7)
    {
    case 0: return DirectLeaf(value);
    case 1: return DirectMiddle(value);
    case 2: return BranchLeft(value);
    case 3: return BranchRight(value);
    case 4: return CrossCallee(value);
    case 5: return ThirdPartyOpaque(value);
    case 6: return DirectEntry(value);
    default: return RecursiveEntry(value & 3);
    }
}

extern "C" __attribute__((noinline)) int TailTarget(int value)
{
    return value + 5;
}

extern "C" __attribute__((noinline)) int TailEntry(int value)
{
    return TailTarget(value);
}

inline int InlineCandidate(int value) { return value * 3; }
extern "C" int InlineEntry(int value) { return InlineCandidate(value); }

__attribute__((noinline)) int Overload(int value) { return value + 1; }
__attribute__((noinline)) long Overload(long value) { return value + 2; }
template <typename T> __attribute__((noinline)) T TemplateUsed(T value) { return value + 4; }
extern "C" int OverloadEntry(int value)
{
    return Overload(value) + static_cast<int>(Overload(static_cast<long>(value))) + TemplateUsed(value);
}

struct Cleanup
{
    int& result;
    __attribute__((noinline)) ~Cleanup() { result += 1; }
};

__attribute__((noinline)) int ThrowMaybe(int value)
{
    if (value)
        throw value;
    return value;
}

extern "C" int ExceptionEntry(int value)
{
    int result = value;
    Cleanup cleanup{result};
    return ThrowMaybe(value) + result;
}
