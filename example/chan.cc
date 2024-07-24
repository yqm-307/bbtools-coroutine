#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

void ReadOnce()
{
    // 注册一个协程
    bbtco [](){
        // 创建一个Chan
        sync::Chan<int, 65535> a;
        // 创建一个协程向chan中写入
        bbtco [&a](){
            a.Write(1);
        };

        // 创建一个协程读取chan中的值
        int val = 0;
        a.Read(val);
        printf("read value = %d\n", val);
    };

    sleep(1);

}

void MultiWrite()
{
    // 创建一个协程
    bbtco [](){
        sync::Chan<int, 65535> c;

        // 创建10个协程去写chan
        for (int i = 0; i<10; ++i) {
            bbtco [&c](){ c.Write(1); };
        }

        // 这个协程读取10次
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

    // 创建一个协程
    bbtco [](){
        sync::Chan<char, 65535> c;
        // 创建协程写入并关闭，此时会唤醒阻塞在Read操作上的协程
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