#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
using namespace bbt::coroutine;

int main()
{
    const int ncount = 500000;
    auto begin = std::chrono::system_clock::now();
    for (int i = 0; i < ncount; ++i)
    {
        bbtco [](){};
    }

    auto end = std::chrono::system_clock::now();
    printf("插入%d个协程耗时 %ldms\n", ncount, std::chrono::duration_cast<std::chrono::milliseconds>((end-begin)).count());

}