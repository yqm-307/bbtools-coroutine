#include <bbt/coroutine/coroutine.hpp>

void debug_1()
{
    while (true)
    {
        bbt::core::thread::CountDownLatch l{1};
        std::atomic_int i = 0;
        auto copool = bbtco_make_copool(10);
    
        __bbtco_event_regist_with_copool_ex(-1, bbtco_emev_persist, 10, copool)
        [&](auto, short event){
            if (++i < 100)
                return true;
            
            l.Down();
            return false;
        };
    
        l.Wait();
        copool->Release();
        assert(i == 100);
    }
}

int main()
{
    g_scheduler->Start();
    std::atomic_int val = 0;

    debug_1();

    g_scheduler->Stop();
}