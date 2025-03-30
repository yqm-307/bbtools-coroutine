#include <bbt/coroutine/coroutine.hpp>

void debug_1()
{
    auto copool = bbtco_make_copool(10);
    auto cocond = bbtco_make_cocond();
    auto chan = bbt::co::sync::Chan<int, 10>();

    auto succ = bbtco_ev_t_with_copool(10, copool) [&](auto, auto){
        for (int i = 0; i < 100; ++i)
        {
            chan << i;
        }
    };
    Assert(succ == 0);

    bbtco [&](){
        while (true)
        {
            int val = 0;
            chan >> val;
            printf("read: %d\n", val);
            if (val == 99)
                break;
        }
    };


    sleep(2);
}

void debug_2()
{
    bbtco [](){
        while (true)
        {
            printf("======== turn once =========\n");
            auto cond = bbtco_make_cocond();
            auto chan = bbt::co::sync::Chan<int, 10>();
            std::atomic_int count{0};

            bbtco [&](){
                while (true)
                {
                    bbtco_sleep(10);
                    printf("enter once! %d\n", count.load());
                    if (count < 5) {
                        count++;
                        printf("write: %d\n", count.load());
                        chan << count.load();
                    }
                    else
                        break;
                }
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
    };
    sleep(10000);
}

int main()
{
    g_scheduler->Start();
    std::atomic_int val = 0;

    debug_1();
    debug_2();

    g_scheduler->Stop();
}