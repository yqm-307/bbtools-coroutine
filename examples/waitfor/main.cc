#include <cstdio>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

int bbtco_main()
{
    bbtco_wait_for(-1, bbtco_emev_timeout, 10);
    std::puts("main coroutine resumed");

    auto done = Chan<int, 1>();
    if ((bbtco_ev_t(10) [done](int, short) { (void)done->TryWrite(1); }) != 0)
        return 1;

    int value = 0;
    if (done->Read(value) != 0)
        return 1;
    std::puts("timer callback ran");
    return 0;
}

BBTCO_MAIN()
