#include <bbt/coroutine/coroutine.hpp>

int main()
{
    // 启动调度器
    g_scheduler->Start();

    // 创建一个协程池，最大协程数为2
    auto co_pool = bbtco_make_copool(2);

    // 基础用法：提交一个工作到协程池
    co_pool->Submit([]() {
        // 在协程中执行的代码
        bbtco_sleep(500);
        std::cout << "Hello from coroutine! now=" << bbt::core::clock::getnow_str() << std::endl;
    });

    sleep(1);

    // 提交多个工作到协程池。若每个人物都是阻塞的，协程池会被占领完毕
    for (int i = 0; i < 3; ++i) {
        co_pool->Submit([i]() {
            // 在协程中执行的代码
            bbtco_sleep(300);
            std::cout << "Hello from coroutine " << i << "! now=" << bbt::core::clock::getnow_str() << std::endl;
        });
    }

    /**
     * 这里可以观察到奇妙的现象，原因就是协程池中的协程都被挂起了。没有空闲协程了
     * 导致第三个协程在池子中前两个执行完才会执行到。
     * 
     * 所以不要在协程池中提交太多需要挂起的任务，否则会导致协程池被占满。
     */
    sleep(1);

    co_pool->Release(); // 阻塞直到池中所有协程全部退出
    // 关闭调度器
    g_scheduler->Stop();
}