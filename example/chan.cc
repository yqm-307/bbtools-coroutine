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

void CloseNotify()
{

    bbtco [](){
        sync::Chan<char> c;
        bbtco [&c](){
            printf("co2 beg %ld\n", bbt::clock::gettime<>());
            ::sleep(1);
            c.Close();
            printf("co2 end %ld\n", bbt::clock::gettime<>());
        };

        char val;
        c.Read(val);
        printf("co1 end %ld\n", bbt::clock::gettime<>());
    };

    sleep(2);
}

int main()
{
    g_scheduler->Start(true);
    printf("============================ Example 1 ============================\n");
    ReadOnce();
    printf("============================ Example 2 ============================\n");
    MultiWrite();
    printf("============================ Example 3 ============================\n");
    CloseNotify();
    g_scheduler->Stop();
}