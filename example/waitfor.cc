#include <cstddef>
#include <bbt/coroutine/coroutine.hpp>

/**
 * bbtco_wait_for 函数提供了对协程等待的支持。
 *
 * 前置条件：必须运行在协程上下文里。
 * 语义：把当前协程挂起到 scheduler 的 wait path，直到 fd 事件或 timeout 发生。
 */

void CoReadable()
{
    bbt::core::thread::CountDownLatch l{1};

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
        l.Down();
    };

    sleep(1);
    printf("CoReadable: %d, now<%lu>, write a char\n", pipefd[1], bbt::core::clock::gettime<>());
    ::write(pipefd[1], "a", 1);

    l.Wait();
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

int main()
{
    // wait_for 只是 public syntax，仍然依赖 scheduler 先启动。
    g_scheduler->Start();

    printf("=============== readable ================\n");
    CoReadable();
    printf("=============== timeout chan ================\n");
    CoTimeout();

    g_scheduler->Stop();
}