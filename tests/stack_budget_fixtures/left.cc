#include "shared.hpp"

static __attribute__((noinline)) int LocalSame(int value)
{
    volatile char bytes[80];
    bytes[value & 63] = static_cast<char>(value);
    return bytes[value & 63];
}

__attribute__((noinline)) int CrossCallee(int value) { return LocalSame(value) + value; }
extern "C" int LeftEntry(int value) { return LocalSame(value) + 1; }
