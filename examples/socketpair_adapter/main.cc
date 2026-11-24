#include <chrono>
#include <cstdio>

#include <sys/socket.h>
#include <unistd.h>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

using namespace bbt::coroutine;
using namespace std::chrono_literals;

bool ReceiveFd()
{
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::perror("socketpair");
        return false;
    }
    bbtco_defer
    {
        ::close(fds[0]);
        ::close(fds[1]);
    };

    sync::CombinedWaitOptions options;
    options.want_readable = true;
    options.fd = fds[0];
    options.deadline = std::chrono::steady_clock::now() + 1s;
    auto waiter = sync::CoWaiter::Create();
    const auto status =
        waiter->Wait(options, [peer = fds[1]]() { return ::send(peer, "x", 1, MSG_DONTWAIT | MSG_NOSIGNAL) == 1; });
    char value = 0;
    if (status != sync::CombinedWaitStatus::FdReadable || ::recv(fds[0], &value, 1, MSG_DONTWAIT) != 1)
        return false;
    std::printf("fd readable: %c\n", value);
    return true;
}

bool ReceiveCallback()
{
    auto waiter = sync::CoWaiter::Create();
    const WaitOptions options{std::chrono::steady_clock::now() + 1s};
    CoEventValue payload;
    // 模拟第三方提交时同步回调：先登记，再启动操作，不会丢早到的完成通知。
    const auto status =
        waiter->WaitWithCallback(options, [waiter]() { return waiter->Notify(CoEventValue::From(42)) == 0; }, &payload);
    int value = 0;
    if (status != WaitStatus::Completed || !payload.Get(value))
        return false;
    std::printf("callback result: %d\n", value);
    return true;
}

int bbtco_main()
{
    return ReceiveFd() && ReceiveCallback() ? 0 : 1;
}

BBTCO_MAIN()
