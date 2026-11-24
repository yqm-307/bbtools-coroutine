#include <cstdio>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

bool RunJobs()
{
    auto workers = bbtco_make_copool(2);
    auto results = Chan<int, 3>();
    for (int i : {1, 2, 3}) {
        if (workers->Submit([i, results]() { (void)results->TryWrite(i * i); }) != 0)
            return false;
    }
    int total = 0;
    for (int i = 0; i < 3; ++i) {
        int value = 0;
        if (results->Read(value) != 0)
            return false;
        total += value;
    }
    workers->Release();
    std::printf("sum of squares: %d\n", total);
    return true;
}

bool RunOnLargeStack()
{
    constexpr StackRequest large{128 * 1024, true};
    const pool::CoPoolOptions classes{{{large, 1, 2}}};
    auto workers = bbtco_make_copool(classes);
    auto done = Chan<std::size_t, 1>();
    if (workers->Submit(large, [done]() { (void)done->TryWrite(GetLocalCoroutineStackSize()); })
        != pool::CoPoolSubmitStatus::Accepted)
        return false;
    std::size_t bytes = 0;
    if (done->Read(bytes) != 0)
        return false;
    workers->Release();
    std::printf("routed stack: %zu bytes\n", bytes);
    return true;
}

RuntimeOptions Options()
{
    RuntimeOptions options;
    options.default_stack = StackRequest{64 * 1024, true};
    options.stacks = StackConfig{StackPolicy::Strict, {{{64 * 1024, true}, 8}, {{128 * 1024, true}, 2}}, 0, 0};
    return options;
}

int bbtco_main()
{
    return RunJobs() && RunOnLargeStack() ? 0 : 1;
}

BBTCO_MAIN_WITH_OPTIONS(Options())
