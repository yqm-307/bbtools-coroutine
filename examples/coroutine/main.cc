#include <cstdio>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

int bbtco_main()
{
    auto done = Chan<int, 1>();
    bbtco_desc("worker")[done]()
    {
        bbtco_defer
        {
            (void)done->TryWrite(42);
        };
        bbtco_yield;
        bbtco_sleep(10);
    };

    int value = 0;
    if (done->Read(value) != 0)
        return 1;
    std::printf("worker result: %d\n", value);
    return 0;
}

BBTCO_MAIN()
