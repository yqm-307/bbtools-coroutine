#include <bbt/coroutine/coroutine.hpp>

void NotifyOne()
{
    std::cout << "=============NotifyOne example=============" << std::endl;
    auto cocond = bbtco_make_cocond();

    /**
     * cocond可以用来在协程中挂起当前协程，并且
     * 在其他协程中进行唤醒。
     */
    bbtco [&](){
        printf("[%d] before wait\n", bbt::coroutine::GetLocalCoroutineId());

        bbtco [&](){
            /**
             * 这里需要休眠100ms的原因是，我们无法假设新注册的协程是否立即会被执行
             * 但是通过100ms的休眠，尽量确保cocond->Wait()已经被调用，
             */
            bbtco_sleep(100);
            printf("[%d] notify one\n", bbt::coroutine::GetLocalCoroutineId());
            cocond->NotifyOne();  // 唤醒一个挂起的协程
        };

        cocond->Wait();  // 挂起当前协程，等待被唤醒
        printf("[%d] after wait\n", bbt::coroutine::GetLocalCoroutineId());
    };

    sleep(1);
}

void NotifyAll()
{
    std::cout << "=============NotifyAll example=============" << std::endl;

    /**
     * NotifyAll的使用场景是，当有多个协程挂起在同一个cocond上时，
     * 可以通过NotifyAll来唤醒所有挂起的协程。
     */
    auto cocond = bbtco_make_cocond();

    for (int i = 0; i < 2; ++i)
    {
        bbtco [&](){
            printf("[%d] before wait\n", bbt::coroutine::GetLocalCoroutineId());
            cocond->Wait();  // 挂起当前协程，等待被唤醒
            printf("[%d] after wait\n", bbt::coroutine::GetLocalCoroutineId());
        };
    }

    bbtco [&](){
        bbtco_sleep(100);  // 确保所有协程都已经注册并挂起
        cocond->NotifyAll();  // 唤醒所有挂起的协程
        printf("[%d] notify all\n", bbt::coroutine::GetLocalCoroutineId());
    };

    sleep(1);
}

int main()
{
    g_scheduler->Start();


    NotifyOne();
    NotifyAll();

    g_scheduler->Stop();
}