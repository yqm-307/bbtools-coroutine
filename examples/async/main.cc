#include <cstdio>
#include <memory>
#include <utility>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

bbt_co_async(int) Fetch(int value)
{
    return bbt_co_bind(
        [](int input) {
            bbtco_sleep(1);
            return input;
        },
        value);
}

bbt_co_async(std::unique_ptr<int>) BuildAnswer(bool ready)
{
    if (!ready)
        bbt_co_return(std::unique_ptr<int>{});
    const int base = bbt_co_await Fetch(40);
    const int extra = bbt_co_await Fetch(2);
    bbt_co_return(std::make_unique<int>(base + extra));
}

int bbtco_main()
{
    auto answer = bbt_co_await BuildAnswer(true);
    if (!answer)
        return 1;

    auto published = Chan<int, 1>();
    auto operation =
        bbt_co_bind([published](std::unique_ptr<int> value) { (void)published->TryWrite(*value); }, std::move(answer));
    bbt_co_fire std::move(operation);

    int value = 0;
    if (published->Read(value) != 0)
        return 1;
    std::printf("published: %d\n", value);
    return 0;
}

BBTCO_MAIN()
