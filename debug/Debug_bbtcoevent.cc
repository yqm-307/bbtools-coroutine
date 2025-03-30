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

void debug_2()
{
    bbtco [](){
        while (true)
        {
            printf("======== turn once =========\n");
            // for (int i = 0; i < 1000; ++i)
            {
                auto cond = bbtco_make_cocond();
                auto chan = bbt::co::sync::Chan<int, 10>();
                std::atomic_int count{0};
    
                __bbtco_event_regist_ex(-1, bbtco_emev_persist, 10)
                [&](int fd, short events) -> bool
                {
                    printf("enter once!\n", count.load());
                    count++;
                    if (count <= 5) {
                        printf("write: %d\n", count.load());
                        chan << count.load();
                        return true;
                    }
        
                    return false;
                };
        
                for (int i = 0; i < 5; ++i)
                {
                    int val = 0;
                    chan >> val;
                    printf("read: %d\n", val);
                }
                chan.Close();
                Assert(count == 5);
            }
        }
    };
    sleep(10000);
}

int main()
{
    g_scheduler->Start();
    std::atomic_int val = 0;

    // debug_1();
    debug_2();

    g_scheduler->Stop();
}