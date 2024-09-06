#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

// 协程挂起功能是否有问题
void dbg_coroutine_wait()
{
    auto cond = sync::CoWaiter::Create();
    while (true) {
        for (int i = 0; i < 10000; ++i)
            bbtco [&](){
                cond->WaitWithTimeout(1);
            };
        ::sleep(1);
    }
    
    bbtco [&](){
        while (1) {
            cond->Notify();
        }
    };
}

int main()
{
    g_scheduler->Start();

    dbg_coroutine_wait();

    g_scheduler->Stop();
}