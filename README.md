# bbtools-coroutine

## 简介

bbtools-coroutine基于 boost.context 实现的C++协程方案，支持linux平台。并且实现了多线程协程调度器以及线程安全的Chan作为同步手段。目前发布版本均完成了疲劳测试。

## 一、安装
### 第一步：获取依赖

- boost.context

    ubuntu可以直接使用apt安装所有的boost库环境

- bbtools-core

    作者的另一个库，bbtools系列库都会依赖的核心库

### 第二步：拉取本库

### 第三步：编译并安装本库

进入本库主目录，执行build.sh脚本即可

二、例程

下面是协程使用，首先以后台线程模式启动调度器，注册协程并运行。可以根据结果看出程序在两个函数之间切换执行。

``` cpp

#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

void Normal()
{
    bbtco [](){
        printf("i am coroutine!\n");
    };

    sleep(1);
}

void SleepHook()
{
    printf("SleepHook begin\n");

    auto id1 = bbtco [](){
        printf("[%d] sleep before!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
        sleep(1);
        printf("[%d] sleep end!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
    };
    printf("create co1 id=%ld now=%ld\n", id1, bbt::core::clock::now<>().time_since_epoch().count());

    auto id2 = bbtco [](){
        printf("[%d] sleep before!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
        sleep(1);
        printf("[%d] sleep end!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
    };
    printf("create co2 id=%ld now=%ld\n", id2, bbt::core::clock::now<>().time_since_epoch().count());

    sleep(2);

    printf("SleepHook end\n");
}

int main()
{
    g_scheduler->Start();

    Normal();

    SleepHook();

    g_scheduler->Stop();
}

```


执行结果:
``` cpp
i am coroutine!
SleepHook begin
create co1 id=2 now=1719914060440
create co2 id=3 now=1719914060440
[3] sleep before!, now=1719914060440
[2] sleep before!, now=1719914060441
[2] sleep end!, now=1719914061441
[3] sleep end!, now=1719914061441
SleepHook end
```

下面创建一个chan来控制协程，通过执行结果可以看到Chan在读的时候会让出执行权，并在Chan有可读的时候切换回来：

``` cpp
#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
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
            printf("co2 beg %ld\n", bbt::core::clock::gettime<>());
            ::sleep(1);
            c.Close();
            printf("co2 end %ld\n", bbt::core::clock::gettime<>());
        };

        char val;
        c.Read(val);
        printf("co1 end %ld\n", bbt::core::clock::gettime<>());
    };

    sleep(2);
}

int main()
{
    g_scheduler->Start();
    printf("============================ Example 1 ============================\n");
    ReadOnce();
    printf("============================ Example 2 ============================\n");
    MultiWrite();
    printf("============================ Example 3 ============================\n");
    CloseNotify();
    g_scheduler->Stop();
}
```

执行结果：
``` cpp
============================ Example 1 ============================
read value = 1
============================ Example 2 ============================
read value: 1
read value: 1
read value: 1
read value: 1
read value: 1
read value: 1
read value: 1
read value: 1
read value: 1
read value: 1
============================ Example 3 ============================
co2 beg 1719914223148
co2 end 1719914224149
co1 end 1719914224150
```