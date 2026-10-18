#include "shared.hpp"

static __attribute__((noinline)) int LocalSame(int value)
{
    volatile char bytes[160];
    bytes[value & 127] = static_cast<char>(value);
    return bytes[value & 127];
}

extern "C" int RightEntry(int value) { return LocalSame(value) + 2; }
