#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

void Normal()
{
    /**
     * 最基础的使用方式，通过 bbtco 注册一个异步协程任务。
     * 前置条件是 scheduler 已经启动。
     */
    bbtco [](){
        printf("i am a common coroutine!\n");
    };


    /**
        * bbtco_desc 只是可读性包装，不改变调度和 failure 语义。
     */
    bbtco_desc("desc") [](){};

        bool succ = false;
        bbtco_noexcept(&succ) [](){};
        printf("bbtco_noexcept register succ=%d\n", succ ? 1 : 0);

    sleep(1);


    /**
     * 我们还可以通过协程来达到在函数执行过程中让出cpu的效果。
     * 
     * bbtco_yield 只是把当前协程重新交给 scheduler，
     * 不承诺之后在哪个时刻、哪个 worker 上恢复。
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
     * bbtco_sleep 只应该在协程上下文里使用；它挂起当前协程，
     * 而不是阻塞整个 worker 线程。
     */
    bbtco [](){
        printf("[%d] sleep before!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
        bbtco_sleep(1000);
        printf("[%d] sleep end!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
    };

    bbtco [](){
        printf("[%d] sleep before!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
        bbtco_sleep(1000);
        printf("[%d] sleep end!, now=%ld\n", bbt::coroutine::GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());
    };

    sleep(2);

    printf("SleepHook end\n");
}

int main()
{
    // 所有依赖协程调度的 public API 都要求先启动 scheduler。
    g_scheduler->Start();

    Normal();

    Defer();

    SleepHook();

    g_scheduler->Stop();
}