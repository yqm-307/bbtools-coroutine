#include <cstdio>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

int bbtco_main()
{
    auto condition = bbtco_make_cocond();
    auto result = Chan<int, 1>();
    bbtco[condition, result]()
    {
        (void)result->TryWrite(condition->NotifyOne());
    };

    // 仅在这个单 worker 示例中，通知协程不可能早于本协程挂起运行。
    const int wait_status = condition->WaitFor(1000);
    int notify_status = -1;
    if (result->Read(notify_status) != 0 || wait_status != 0 || notify_status != 0)
        return 1;
    std::puts("waiter notified");
    return 0;
}

RuntimeOptions Options()
{
    RuntimeOptions options;
    options.initial_workers = 1;
    return options;
}

BBTCO_MAIN_WITH_OPTIONS(Options())
