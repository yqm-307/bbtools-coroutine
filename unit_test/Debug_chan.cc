#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
// #include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

int main()
{
    // g_scheduler->Start(true);

    // bbtco [](){
    //     sync::Chan a;
    //     bbtco [&a](){
    //         a.Write(1);
    //     };

    //     int val = 0;
    //     a.Read(val);
    //     printf("read value = %d\n", val);
    // };

    // sleep(1);

    // g_scheduler->Stop();
}