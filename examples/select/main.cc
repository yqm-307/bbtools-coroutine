#include <cstdio>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoSelect.hpp>

using namespace bbt::coroutine;

int bbtco_main()
{
    auto left = Chan<int, 1>();
    auto right = Chan<int, 1>();
    auto written = Chan<int, 1>();
    bbtco[right, written]()
    {
        (void)written->TryWrite(right->Write(42));
    };

    // 两个通道都由主协程保活，示例不允许在选择期间关闭通道。
    int value = 0;
    const int index = sync::CoSelect().CaseRead(left, value).CaseRead(right, value).CaseTimeout(1000).Run();
    int status = -1;
    if (written->Read(status) != 0 || status != 0)
        return 1;
    if (index < 0) {
        std::fputs("no case selected\n", stderr);
        return 1;
    }
    std::printf("case %d: %d\n", index, value);
    return 0;
}

BBTCO_MAIN()
