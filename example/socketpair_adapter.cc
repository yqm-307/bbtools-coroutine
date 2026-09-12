/**
 * @file socketpair_adapter.cc
 * @brief 第三方库 fd 事件适配示例（socketpair 模拟，不依赖真实外部服务）
 *
 * 展示模式：
 * 1. fd 类库：拿 fd → YieldUntilFdReadable → 就绪后处理数据
 * 2. 回调类库：外部线程回调 → CoWaiter::Notify → 协程恢复
 *
 * 真实场景中将 socketpair 替换为 hiredis-async 的 fd，
 * 将 handle_read 替换为 redisAsyncHandleRead。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

using namespace bbt::coroutine;

static void fd_adapter_example()
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        perror("socketpair");
        return;
    }

    std::atomic<bool> got_data{false};

    /* 协程：挂起等 fd 可读 */
    bbtco [fds, &got_data]() {
        /* 等价于 hiredis-async 的 redisAsyncHandleRead 前的等待 */
        int ret = g_bbt_tls_coroutine_co->YieldUntilFdReadable(fds[0], 5000);
        if (ret == 0) {
            char buf[64];
            ssize_t n = read(fds[0], buf, sizeof(buf));
            if (n > 0) {
                got_data.store(true);
                printf("[fd_adapter] read %zd bytes: %.*s\n", n, (int)n, buf);
            }
        } else {
            printf("[fd_adapter] timeout or error: %d\n", ret);
        }
    };

    /* 模拟第三方库异步写入数据 */
    std::thread writer([fds]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const char* msg = "hello from adapter";
        write(fds[1], msg, 19);
    });

    /* 等协程完成 */
    for (int i = 0; i < 100 && !got_data.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    writer.join();
    close(fds[0]);
    close(fds[1]);

    if (got_data.load())
        printf("[fd_adapter] PASS\n");
    else
        printf("[fd_adapter] FAIL: no data received\n");
}

static void custom_event_adapter_example()
{
    auto waiter = sync::CoWaiter::Create();
    std::atomic<bool> notified{false};

    /* 协程：挂起等外部事件 */
    bbtco [waiter, &notified]() {
        int ret = waiter->WaitWithTimeout(5000);
        if (ret == 0) {
            notified.store(true);
            printf("[custom_event] notified\n");
        } else {
            printf("[custom_event] timeout: %d\n", ret);
        }
    };

    /* 模拟第三方库回调线程 */
    std::thread notifier([waiter]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waiter->Notify();
    });

    for (int i = 0; i < 100 && !notified.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    notifier.join();

    if (notified.load())
        printf("[custom_event] PASS\n");
    else
        printf("[custom_event] FAIL: not notified\n");
}

int main()
{
    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    if (!g_scheduler->IsRunning()) {
        fprintf(stderr, "scheduler start failed\n");
        return 1;
    }

    printf("=== fd adapter (socketpair) ===\n");
    fd_adapter_example();

    printf("=== custom event adapter ===\n");
    custom_event_adapter_example();

    g_scheduler->Stop();
    printf("done\n");
    return 0;
}
