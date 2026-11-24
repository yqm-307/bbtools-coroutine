#include <cstdio>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

template <int Capacity> bool ReceiveJobs()
{
    auto jobs = Chan<int, Capacity>();
    bbtco[jobs]()
    {
        bbtco_defer
        {
            jobs->Close();
        };
        for (int value : {10, 20, 30}) {
            if (jobs->Write(value) != 0)
                return;
        }
    };

    int value = 0;
    int status = 0;
    int total = 0;
    while ((status = jobs->Read(value)) == 0)
        total += value;
    if (status != -1)
        return false;

    std::printf("capacity %d: total %d\n", Capacity, total);
    return true;
}

int bbtco_main()
{
    return ReceiveJobs<2>() && ReceiveJobs<0>() ? 0 : 1;
}

BBTCO_MAIN()
