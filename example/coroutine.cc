#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

void Normal()
{
    /**
     * 最基础的使用方式，通过bbtco 来创建一个协程。
     * 可能在之后的任意时刻执行此函数。
     */
    bbtco [](){
        printf("i am a common coroutine!\n");
    };


    /**
     * bbtco_desc可以增加可读性，后续考虑保存在Coroutine中去
     * 
     * 完全等同于
     * 
     * bbtco [](){};
     */
    bbtco_desc("desc") [](){};

    sleep(1);


    /**
     * 我们还可以通过协程来达到在函数执行过程中让出cpu的效果。
     * 
     * 使得两个协程交替执行。
     * 这种方式可以用来模拟协程的协作式调度。但是执行顺序是不保证的
     * 
     * ps：重要的事情说三遍
     * 
     *  注册后的协程不保证执行顺序！！！
     *  注册后的协程不保证执行顺序！！！
     *  注册后的协程不保证执行顺序！！！
     */
    bbtco [](){
        printf("i am common coroutine %lu\n", bbt::co::GetLocalCoroutineId());
        bbtco_yield;
        printf("i am common coroutine %lu after yield\n", bbt::co::GetLocalCoroutineId());
    };

    bbtco [](){
        printf("i am common coroutine %lu, i will yield now\n", bbt::co::GetLocalCoroutineId());
        bbtco_yield;
        printf("i am common coroutine %lu after yield\n", bbt::co::GetLocalCoroutineId());
    };

    sleep(1);
}

void Defer()
{
    struct Obj{
        Obj() { printf("Obj created\n"); }
        ~Obj() { printf("Obj destroyed\n"); }
    };

    struct Obj2{
        Obj2() { printf("Obj2 created\n"); }
        ~Obj2() { printf("Obj2 destroyed\n"); }
    };

    /**
     * bbtco_defer可以用来在协程结束时自动执行一些操作。
     * 这种方式可以用来模拟C++的RAII特性。
     */
    bbtco [](){
        Obj* obj = new Obj;
        bbtco_defer { delete obj; };
    };

    /**
     * bbtco_defer 释放顺序和defer声明顺序相反
     * 
     * 并且在return后才会执行defer。
     * 
     * defer实际上就是构造了一个临时对象，这个
     * 临时对象释放时构造函数中执行defer操作。
     */
    bbtco [](){
        bbtco_defer { printf("defer 1\n"); };
        bbtco_defer { printf("defer 2\n"); };
        return [](){printf("return\n");}();
    };

    /**
     * 因为析构是根据函数调用栈的逆序执行的，
     * 所以在协程中使用defer时，defer的执行顺序
     * 是和它们被声明的顺序相反的。
     * 
     * 同时最佳的使用方式，就是在资源创建后，紧跟
     * 需要执行释放的defer。
     */
    bbtco [](){
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        bbtco_defer { close(fd); };  // 在协程结束时自动关闭文件描述符
    };

}

void SleepHook()
{
    printf("SleepHook begin\n");

    /**
     * bbtco_sleep可以用来在协程中挂起当前协程，
     * 使得其他协程可以继续执行。
     * 
     * 注意：在Processer线程中，sleep会挂起当前协程，
     * 而不是整个线程。
     */
    bbtco [](){
        printf("[%d] sleep before!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
        sleep(1);
        printf("[%d] sleep end!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
    };

    bbtco [](){
        printf("[%d] sleep before!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
        sleep(1);
        printf("[%d] sleep end!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
    };

    sleep(2);

    printf("SleepHook end\n");
}

int main()
{
    g_scheduler->Start();

    Normal();

    Defer();

    SleepHook();

    g_scheduler->Stop();
}