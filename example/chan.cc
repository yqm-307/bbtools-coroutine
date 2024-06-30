#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
// #include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

void ReadOnce()
{

    bbtco [](){
        sync::Chan<int> a;
        bbtco [&a](){
            a.Write(1);
        };

        int val = 0;
        a.Read(val);
        printf("read value = %d\n", val);
    };

    sleep(1);

}

void MultiWrite()
{
    bbtco [](){
        sync::Chan<int> c;
        for (int i = 0; i<10; ++i) {
            bbtco [&c](){ c.Write(1); };
        }

        for (int i = 0; i < 10; ++i)
        {
            int value = 0;
            c.Read(value);
            printf("read value: %d\n", value);
        }
    };

    sleep(1);
}

int main()
{
    g_scheduler->Start(true);

    ReadOnce();

    g_scheduler->Stop();
}