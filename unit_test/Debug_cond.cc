#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
using namespace bbt::coroutine;


#define PrintTime(flag) printf("标记点=[%s]   协程id=[%ld] 时间戳=[%ld]\n", flag, GetLocalCoroutineId(), bbt::clock::now<>().time_since_epoch().count());

int main()
{
    /* 启动调度线程、Processer线程 */
    g_scheduler->Start(true);

    PrintTime("t1");

    /* 注册一个 coroutine */
    g_scheduler->RegistCoroutineTask([](){
        PrintTime("t2");
        /* 创建条件变量 */
        auto cond = sync::CoCond::Create();
        Assert(cond != nullptr);

        /* 注册一个coroutine */
        g_scheduler->RegistCoroutineTask([&cond](){
            PrintTime("t4");
            /* 协程休眠1s后唤醒等待中的条件变量 */
            ::sleep(1);
            cond->Notify();
        });   

        PrintTime("t3");
        /* 让当前协程等待条件变量变化 */
        cond->Wait();
        PrintTime("t5");
    });

    std::this_thread::sleep_for(bbt::clock::milliseconds(2000));
    g_scheduler->Stop();
    return 0;
}
