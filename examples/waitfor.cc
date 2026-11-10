#include <atomic>
#include <cstddef>
#include <bbt/coroutine/coroutine.hpp>

/**
 * bbtco_wait_for 函数提供了对协程等待的支持。
 * 
 * 使用起来非常简单，通过参数告知等待的fd、事件类型和超时时间（可选）
 * 即可让当前协程等待直到事件发生后被唤醒。
 */

void CoReadable()
{
    std::atomic_bool done{false};

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        return;
    }
    printf("co_%lu write succ time<%ld>\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>());

    bbtco_ref {
        printf("CoReadable: %d, now<%lu>\n", pipefd[0], bbt::core::clock::gettime<>());
        bbtco_wait_for(pipefd[0], bbtco_emev_readable, 0);
        char rlt[12] = {0};
        ::read(pipefd[0], rlt, sizeof(rlt));
        printf("CoReadable: %d, now<%lu>, readsucc: %s\n", pipefd[0], bbt::core::clock::gettime<>(), rlt);
        done.store(true, std::memory_order_release);
    };

    sleep(1);
    printf("CoReadable: %d, now<%lu>, write a char\n", pipefd[1], bbt::core::clock::gettime<>());
    ::write(pipefd[1], "a", 1);

    // 完成可能先于等待；保留状态并让出，不能用原生 latch 占住 worker。
    while (!done.load(std::memory_order_acquire))
        bbtco_sleep(1);
}

void CoTimeout()
{
    bbtco_ref {
        printf("CoTimeout: %lu, now<%lu>\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>());
        bbtco_wait_for(0, bbtco_emev_timeout, 500); // 等待0.5秒超时
        printf("CoTimeout: %lu, now<%lu>, timeout\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>());
        bbtco_wait_for(0, bbtco_emev_timeout, 100); // 等待0.1秒超时
        printf("CoTimeout: %lu, now<%lu>, timeout again\n", bbt::co::GetLocalCoroutineId(), bbt::core::clock::gettime<>());
    };

    sleep(1);
}

int bbtco_main() {
    printf("=============== readable ================\n");
    CoReadable();
    printf("=============== timeout chan ================\n");
    CoTimeout();

    /* process-lifetime：无业务停机入口；业务完成后返回 main 即进程退出 */
    return 0;
}

BBTCO_MAIN()