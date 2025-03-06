#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

void Pool()
{
    pool::CoPool pool{100};
    const int max_co = 100000;
    bbt::core::thread::CountDownLatch l{max_co};

    for (int i = 0; i < max_co; ++i) {
        pool.Submit([&](){
            Assert(GetLocalCoroutineId() > 0);
            l.Down();
        });
    }

    l.Wait();

}

int main()
{
    g_scheduler->Start();

    Pool();

    g_scheduler->Stop();
}