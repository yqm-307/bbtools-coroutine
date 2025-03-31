# bbtools-coroutine

## 简介

bbtools-coroutine基于 boost.context 实现的C++协程方案，支持linux平台。并且实现了多线程协程调度器以及线程安全的Chan作为同步手段。目前发布版本均完成了疲劳测试。

## 一、安装
- boost.context

    ```shell
    sudo apt install libboost-all-dev
    ```

- bbtools-core

    ```shell
    git clone https://github.com/yqm-307/bbtools-core.git
    cd bbtools-core
    cd shell
    sudo ./build.sh
    ```
    
- libevent

    ```shell
    sudo apt install libevent-dev
    ```

- bbtools_coroutine

    ```shell
    git clone ${库地址}
    cd ${库文件夹}
    sudo ./build.sh
    ```

## 二、例程

下面是协程使用，首先以后台线程模式启动调度器，注册协程并运行。可以根据结果看出程序在两个函数之间切换执行。

``` cpp
#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
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


执行结果:
``` cpp
i am coroutine!
SleepHook begin
create co1 id=2 now=1743432500565
[2] sleep before!, now=1743432500565
create co2 id=3 now=1743432500565
[3] sleep before!, now=1743432500573
[2] sleep end!, now=1743432501590
[3] sleep end!, now=1743432501590
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
co2 beg 1743432623131
co2 end 1743432624160
co1 end 1743432624160
```

## 三、性能对比，libgo，go

这里看下不同频率的cpu下，执行100w协程的耗时。这里内存频率也不同，会导致误差，仅供参考。不过因为bbtco内使用了无锁队列，所以cpu频率越高、内存频率越高，框架整体协程调度效率越高。

|       | Intel(R) Xeon(R) CPU E5-26xx v4（2.1Ghz） | Intel(R) Core(TM) Ultra 7 155H | 13th Gen Intel(R) Core(TM) i7-13790F |
| ----- | ----------------------------------------- | ------------------------------ | ------------------------------------ |
| libgo | 1929 ms                                   | 4878 ms                        | 3828 ms                              |
| bbtco | 3959 ms                                   | 3623 ms                        | 1307 ms                              |
| go    | 641 ms                                    | 600 ms                         | 357 ms                               |

