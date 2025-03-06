#include <bbt/coroutine/coroutine.hpp>

int main()
{
    const int sec = 5;
    auto end = bbt::core::clock::nowAfter<>(bbt::core::clock::ms(sec * 1000));
    bbt::core::thread::CountDownLatch l{1};

    g_scheduler->Start();

    bbtco_desc("main") [end, &l](){

        auto comutex = bbtco_make_comutex();
        int a=0, b=0;

        bbtco_desc("productor") [&, comutex, end](){
            while (!bbt::core::clock::is_expired<bbt::core::clock::ms>(end)) {
                comutex->Lock();
                a++;
                b++;
                comutex->UnLock();
            }
        };


        bbtco_desc("consumer") [&, comutex, end](){
            while (!bbt::core::clock::is_expired<bbt::core::clock::ms>(end)) {
                comutex->Lock();
                Assert(a == b);
                comutex->UnLock();
            }
        };
        

        while (!bbt::core::clock::is_expired<bbt::core::clock::ms>(end)) {
            comutex->Lock();
            printf("current a=%d b=%d\n", a, b);
            comutex->UnLock();

            bbtco_sleep(500);
        }

        l.Down();
    };

    l.Wait();
    g_scheduler->Stop();
}