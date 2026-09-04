#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

// #232 hook 行为契约测试基座。
// 铁律：① 非协程上下文直通原函数 ② 协程内 fd 已就绪立即返回（不挂死）
// ③ 失败路径 errno 保持（EBADF 等） ④ EINTR 可构造。
// 11 个 hook 各一个用例；新 hook 直接复制一个 BOOST_AUTO_TEST_CASE 即可，无需框架。

#include "hook_contract.hpp"

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/thread/Lock.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <sys/uio.h>

using hook_contract::BindLoopbackPort0;
using hook_contract::EintrArm;
using hook_contract::FdGuard;
using hook_contract::IsNonblock;
using hook_contract::RunInCo;
using hook_contract::SetNonblock;
using hook_contract::SocketPair;

BOOST_AUTO_TEST_SUITE(HookContract)

BOOST_AUTO_TEST_CASE(test_env_setup)
{
    g_scheduler->Start();
}

// socket：协程内 fd 带 O_NONBLOCK（hook 挂起调度的前提）；测试线程（非协程）直通不带
BOOST_AUTO_TEST_CASE(t_contract_socket)
{
    RunInCo([&]() {
        FdGuard fd{::socket(AF_INET, SOCK_STREAM, 0)};
        BOOST_REQUIRE(fd.Get() >= 0);
        BOOST_TEST(IsNonblock(fd.Get()));
    });

    int raw_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(raw_fd >= 0);
    // 直通路径：原生 fd 不应被偷偷改成非阻塞
    BOOST_TEST(!IsNonblock(raw_fd));
    BOOST_CHECK_EQUAL(::close(raw_fd), 0);
}

// close：协程/非协程 close 合法 fd 均为 0；close(-1) = -1 且 errno=EBADF 保持
BOOST_AUTO_TEST_CASE(t_contract_close)
{
    std::atomic_bool co_ran{false};
    RunInCo([&]() {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_REQUIRE(fd >= 0);
        BOOST_CHECK_EQUAL(::close(fd), 0);

        errno = 0;
        BOOST_CHECK_EQUAL(::close(-1), -1);
        BOOST_CHECK_EQUAL(errno, EBADF);
        co_ran = true;
    });
    BOOST_TEST(co_ran.load());

    int raw_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(raw_fd >= 0);
    BOOST_CHECK_EQUAL(::close(raw_fd), 0);
    errno = 0;
    BOOST_CHECK_EQUAL(::close(-1), -1);
    BOOST_CHECK_EQUAL(errno, EBADF);
}

// sleep：非协程 sleep(0) 直通立即返回；协程 sleep(1) 挂起期间 ticker 协程计数增长。
// WHY 协程路径只留这一个用例：sleep(1) 是证明"挂起的是协程不是线程"的最小可观测
// 时长，每个用例再付 1 秒只会拖慢全量 CTest。
BOOST_AUTO_TEST_CASE(t_contract_sleep)
{
    auto begin = std::chrono::steady_clock::now();
    BOOST_CHECK_EQUAL(::sleep(0), 0u);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();
    BOOST_TEST(elapsed_ms < 1000);

    std::atomic<int> ticks{0};
    std::atomic_bool stop{false};
    RunInCo([&]() {
        bbtco [&]() {
            while (!stop.load()) {
                bbtco_sleep(10);
                ++ticks;
            }
        };
        ::sleep(1);  // 协程 hook：YieldUntilTimeout(1000ms)，让调度器跑 ticker
        stop = true;
        // ticker 最长再睡 10ms；等它退出循环，避免用例栈帧析构后访问悬挂引用
        bbtco_sleep(50);
    });
    BOOST_TEST(ticks.load() > 0);
}

// read：协程阻塞（空 socketpair + 延迟写 + ticker）、协程就绪（先写后读立即返回）、
// 非协程直通；失败路径 EBADF 保持
BOOST_AUTO_TEST_CASE(t_contract_read)
{
    // 就绪立即返回：写满缓冲后再进协程读，不应挂起（写走原生 fd，语义无歧义）
    RunInCo([&]() {
        SocketPair sp;
        BOOST_REQUIRE(sp.Ok());
        BOOST_REQUIRE(SetNonblock(sp[0]));
        const char msg[] = "ready";
        BOOST_REQUIRE(::write(sp[1], msg, sizeof(msg)) == (ssize_t)sizeof(msg));
        char buf[16] = {0};
        BOOST_CHECK(::read(sp[0], buf, sizeof(buf)) == (ssize_t)sizeof(msg));
        BOOST_TEST(std::string{buf} == std::string{msg});
    });

    // 挂起等待：空 socketpair 上 read 挂起，另一协程延迟写入后唤醒
    std::atomic<int> ticker_writes{0};
    RunInCo([&]() {
        SocketPair sp;
        BOOST_REQUIRE(sp.Ok());
        BOOST_REQUIRE(SetNonblock(sp[0]));
        bbtco [&]() {
            bbtco_sleep(50);
            // WHY 先自增再写：read 唤醒即可能让主协程先结束用例，++ 必须发生在
            // 唤醒之前，否则 ticker 协程会写用例栈上已析构的变量
            ++ticker_writes;
            ::write(sp[1], "wake", 4);
        };
        char buf[16] = {0};
        ssize_t got = ::read(sp[0], buf, sizeof(buf));
        BOOST_CHECK_EQUAL(got, 4);
        BOOST_TEST(std::string(buf, 4) == "wake");
    });
    BOOST_TEST(ticker_writes.load() == 1);

    // errno 保持：协程内无效 fd
    RunInCo([&]() {
        errno = 0;
        char buf[8];
        BOOST_CHECK_EQUAL(::read(-1, buf, sizeof(buf)), -1);
        BOOST_CHECK_EQUAL(errno, EBADF);
    });

    // 非协程直通：先写后读
    SocketPair sp;
    BOOST_REQUIRE(sp.Ok());
    const char msg[] = "direct";
    BOOST_REQUIRE(::write(sp[1], msg, sizeof(msg)) == (ssize_t)sizeof(msg));
    char buf[16] = {0};
    BOOST_CHECK(::read(sp[0], buf, sizeof(buf)) == (ssize_t)sizeof(msg));
    BOOST_TEST(std::string{buf} == std::string{msg});
}

// write：协程内对空缓冲 fd 立即成功返回；非协程直通
BOOST_AUTO_TEST_CASE(t_contract_write)
{
    RunInCo([&]() {
        SocketPair sp;
        BOOST_REQUIRE(sp.Ok());
        BOOST_REQUIRE(SetNonblock(sp[1]));
        const char msg[] = "co-write";
        // 缓冲空 → 原生 write 立即成功，hook 不挂起；对端原生 fd 读回
        BOOST_CHECK(::write(sp[1], msg, sizeof(msg)) == (ssize_t)sizeof(msg));
        char buf[16] = {0};
        BOOST_REQUIRE(::read(sp[0], buf, sizeof(buf)) == (ssize_t)sizeof(msg));
        BOOST_TEST(std::string{buf} == std::string{msg});
    });

    SocketPair sp;
    BOOST_REQUIRE(sp.Ok());
    const char msg[] = "direct-write";
    BOOST_CHECK(::write(sp[1], msg, sizeof(msg)) == (ssize_t)sizeof(msg));
    char buf[16] = {0};
    BOOST_CHECK(::read(sp[0], buf, sizeof(buf)) == (ssize_t)sizeof(msg));
    BOOST_TEST(std::string{buf} == std::string{msg});
}

// send/recv：socketpair 就绪路径，协程立即返回 + 非协程直通
BOOST_AUTO_TEST_CASE(t_contract_send_recv)
{
    RunInCo([&]() {
        SocketPair sp;
        BOOST_REQUIRE(sp.Ok());
        BOOST_REQUIRE(SetNonblock(sp[0]));
        BOOST_REQUIRE(SetNonblock(sp[1]));
        const char msg[] = "co-send";
        BOOST_CHECK(::send(sp[1], msg, sizeof(msg), 0) == (ssize_t)sizeof(msg));
        char buf[16] = {0};
        BOOST_CHECK(::recv(sp[0], buf, sizeof(buf), 0) == (ssize_t)sizeof(msg));
        BOOST_TEST(std::string{buf} == std::string{msg});
    });

    SocketPair sp;
    BOOST_REQUIRE(sp.Ok());
    const char msg[] = "direct-send";
    BOOST_CHECK(::send(sp[1], msg, sizeof(msg), 0) == (ssize_t)sizeof(msg));
    char buf[16] = {0};
    BOOST_CHECK(::recv(sp[0], buf, sizeof(buf), 0) == (ssize_t)sizeof(msg));
    BOOST_TEST(std::string{buf} == std::string{msg});
}

// sendto/recvfrom：UDP loopback 就绪路径，协程 + 非协程（port 0 内核分配）
BOOST_AUTO_TEST_CASE(t_contract_sendto_recvfrom)
{
    RunInCo([&]() {
        FdGuard server{::socket(AF_INET, SOCK_DGRAM, 0)};
        FdGuard client{::socket(AF_INET, SOCK_DGRAM, 0)};
        BOOST_REQUIRE(server.Get() >= 0 && client.Get() >= 0);
        sockaddr_in addr;
        BOOST_REQUIRE(BindLoopbackPort0(server.Get(), &addr));

        const char msg[] = "udp-co";
        BOOST_CHECK(::sendto(client.Get(), msg, sizeof(msg), 0,
                             (struct sockaddr*)&addr, sizeof(addr)) == (ssize_t)sizeof(msg));
        // 数据已在队列：recvfrom 应就绪立即返回
        char buf[32] = {0};
        BOOST_CHECK(::recvfrom(server.Get(), buf, sizeof(buf), 0, nullptr, nullptr) == (ssize_t)sizeof(msg));
        BOOST_TEST(std::string{buf} == std::string{msg});
    });

    FdGuard server{::socket(AF_INET, SOCK_DGRAM, 0)};
    FdGuard client{::socket(AF_INET, SOCK_DGRAM, 0)};
    BOOST_REQUIRE(server.Get() >= 0 && client.Get() >= 0);
    sockaddr_in addr;
    BOOST_REQUIRE(BindLoopbackPort0(server.Get(), &addr));
    const char msg[] = "udp-direct";
    BOOST_CHECK(::sendto(client.Get(), msg, sizeof(msg), 0,
                         (struct sockaddr*)&addr, sizeof(addr)) == (ssize_t)sizeof(msg));
    char buf[32] = {0};
    BOOST_CHECK(::recvfrom(server.Get(), buf, sizeof(buf), 0, nullptr, nullptr) == (ssize_t)sizeof(msg));
    BOOST_TEST(std::string{buf} == std::string{msg});
}

// connect：监听 port 0；协程 loopback 建连成功；非协程直通成功；无效 fd errno 保持
BOOST_AUTO_TEST_CASE(t_contract_connect)
{
    FdGuard listen_fd{::socket(AF_INET, SOCK_STREAM, 0)};
    BOOST_REQUIRE(listen_fd.Get() >= 0);
    sockaddr_in addr;
    BOOST_REQUIRE(BindLoopbackPort0(listen_fd.Get(), &addr));
    BOOST_CHECK_EQUAL(::listen(listen_fd.Get(), 4), 0);

    RunInCo([&]() {
        FdGuard fd{::socket(AF_INET, SOCK_STREAM, 0)};
        BOOST_REQUIRE(fd.Get() >= 0);
        // backlog 有空位，loopback 建连完成；协程 connect 不应挂死
        BOOST_CHECK_MESSAGE(::connect(fd.Get(), (struct sockaddr*)&addr, sizeof(addr)) == 0,
                            "errno=" << errno);
        // 失败路径 errno：协程内无效 fd
        errno = 0;
        BOOST_CHECK_EQUAL(::connect(-1, (struct sockaddr*)&addr, sizeof(addr)), -1);
        BOOST_CHECK_EQUAL(errno, EBADF);
    });

    // 非协程直通 connect（backlog 未满，无需 acceptor 配合）
    {
        FdGuard fd{::socket(AF_INET, SOCK_STREAM, 0)};
        BOOST_REQUIRE(fd.Get() >= 0);
        BOOST_CHECK_EQUAL(::connect(fd.Get(), (struct sockaddr*)&addr, sizeof(addr)), 0);
    }
}

// accept：协程挂起直到 connect 到达（listen fd 需先置非阻塞，hook 才有 EAGAIN→挂起路径）；
// 非协程 backlog 先 connect 再 accept 立即成功
BOOST_AUTO_TEST_CASE(t_contract_accept)
{
    FdGuard listen_fd{::socket(AF_INET, SOCK_STREAM, 0)};
    BOOST_REQUIRE(listen_fd.Get() >= 0);
    sockaddr_in addr;
    BOOST_REQUIRE(BindLoopbackPort0(listen_fd.Get(), &addr));
    BOOST_CHECK_EQUAL(::listen(listen_fd.Get(), 4), 0);

    std::atomic_bool client_done{false};
    RunInCo([&]() {
        // WHY 置非阻塞：listen fd 是测试线程原生创建的，协程 hook 只覆盖 socket()，
        // 不置非阻塞则 Hook_Accept 的原生 accept 会阻塞执行线程而非走挂起路径
        BOOST_REQUIRE(SetNonblock(listen_fd.Get()));
        bbtco [&]() {
            bbtco_sleep(50);
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd >= 0) {
                if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0)
                    client_done = true;
                ::close(fd);
            }
        };
        int cli = ::accept(listen_fd.Get(), nullptr, nullptr);
        BOOST_REQUIRE(cli >= 0);
        BOOST_CHECK(IsNonblock(cli));  // 协程路径：accept 出的新 fd 也被 hook 置非阻塞
        ::close(cli);
        // 等 client 协程把 client_done 写定：内核在握手完成时即唤醒 accept，
        // 可能早于 client 协程的 connect() 返回并置位
        bbtco_sleep(50);
    });
    BOOST_TEST(client_done.load());

    // 非协程直通：独立阻塞 listener，backlog 先有连接再 accept
    FdGuard blocky{::socket(AF_INET, SOCK_STREAM, 0)};
    BOOST_REQUIRE(blocky.Get() >= 0);
    sockaddr_in baddr;
    BOOST_REQUIRE(BindLoopbackPort0(blocky.Get(), &baddr));
    BOOST_CHECK_EQUAL(::listen(blocky.Get(), 4), 0);
    {
        FdGuard cli{::socket(AF_INET, SOCK_STREAM, 0)};
        BOOST_REQUIRE(cli.Get() >= 0);
        BOOST_CHECK_EQUAL(::connect(cli.Get(), (struct sockaddr*)&baddr, sizeof(baddr)), 0);
        int srv = ::accept(blocky.Get(), nullptr, nullptr);  // backlog 已有连接，直通立即成功
        BOOST_REQUIRE(srv >= 0);
        BOOST_TEST(!IsNonblock(srv));  // 直通路径：新 fd 未被 hook 改动
        ::close(srv);
    }
}

// EINTR：仅需证明构造器可用——非协程空 pipe 阻塞 read + EintrArm → -1/EINTR（hook 循环
// 重试 EINTR 已由现实现保证，协程内信号投递到任意线程、不可靠定位，不做重试断言）。
BOOST_AUTO_TEST_CASE(t_contract_eintr)
{
    int fds[2];
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);
    FdGuard r{fds[0]};
    FdGuard w{fds[1]};
    {
        EintrArm arm{20000};  // 20ms 后 SIGALRM（无 SA_RESTART），打断下方阻塞 read
        char buf[8];
        errno = 0;
        ssize_t got = ::read(r.Get(), buf, sizeof(buf));
        BOOST_CHECK_EQUAL(got, -1);
        BOOST_CHECK_EQUAL(errno, EINTR);
    }
    // 解除后同 fd 恢复正常读写
    const char msg[] = "ok";
    BOOST_REQUIRE(::write(w.Get(), msg, sizeof(msg)) == (ssize_t)sizeof(msg));
    char buf[8] = {0};
    BOOST_CHECK(::read(r.Get(), buf, sizeof(buf)) == (ssize_t)sizeof(msg));
}

// sendmsg/recvmsg：单段 iovec；协程就绪立即返回、空 fd 挂起+延迟写唤醒、
// MSG_DONTWAIT 空 socketpair 协程内直通 -1/EAGAIN、非协程直通、EBADF 保持
BOOST_AUTO_TEST_CASE(t_contract_sendmsg_recvmsg)
{
    // 就绪立即返回（对端原生 fd 写入）
    RunInCo([&]() {
        SocketPair sp;
        BOOST_REQUIRE(sp.Ok());
        BOOST_REQUIRE(SetNonblock(sp[0]));
        const char msg[] = "co-sendmsg";
        BOOST_REQUIRE(::write(sp[1], msg, sizeof(msg)) == (ssize_t)sizeof(msg));

        char buf[16] = {0};
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        struct msghdr mh;
        std::memset(&mh, 0, sizeof(mh));
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        BOOST_CHECK(::recvmsg(sp[0], &mh, 0) == (ssize_t)sizeof(msg));
        BOOST_TEST(std::string{buf} == std::string{msg});
    });

    // 协程 sendmsg 写空缓冲立即成功，对端原生读回
    RunInCo([&]() {
        SocketPair sp;
        BOOST_REQUIRE(sp.Ok());
        BOOST_REQUIRE(SetNonblock(sp[1]));
        const char msg[] = "co-sendmsg2";
        struct iovec iov;
        iov.iov_base = const_cast<char*>(msg);
        iov.iov_len = sizeof(msg);
        struct msghdr mh;
        std::memset(&mh, 0, sizeof(mh));
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        BOOST_CHECK(::sendmsg(sp[1], &mh, 0) == (ssize_t)sizeof(msg));
        char buf[16] = {0};
        BOOST_REQUIRE(::read(sp[0], buf, sizeof(buf)) == (ssize_t)sizeof(msg));
        BOOST_TEST(std::string{buf} == std::string{msg});
    });

    // 挂起等待：空 socketpair 上 recvmsg 挂起，ticker 协程延迟原生写后唤醒
    std::atomic<int> ticker_writes{0};
    RunInCo([&]() {
        SocketPair sp;
        BOOST_REQUIRE(sp.Ok());
        BOOST_REQUIRE(SetNonblock(sp[0]));
        bbtco [&]() {
            bbtco_sleep(50);
            // WHY 先自增再写：同 t_contract_read，防止 ticker 写已析构栈变量
            ++ticker_writes;
            ::write(sp[1], "wake", 4);
        };
        char buf[16] = {0};
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        struct msghdr mh;
        std::memset(&mh, 0, sizeof(mh));
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        BOOST_CHECK_EQUAL(::recvmsg(sp[0], &mh, 0), 4);
        BOOST_TEST(std::string(buf, 4) == "wake");
    });
    BOOST_TEST(ticker_writes.load() == 1);

    // MSG_DONTWAIT：协程内也直通，空 socketpair 立即 -1/EAGAIN，不挂死
    RunInCo([&]() {
        SocketPair sp;
        BOOST_REQUIRE(sp.Ok());
        BOOST_REQUIRE(SetNonblock(sp[0]));
        char buf[16];
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        struct msghdr mh;
        std::memset(&mh, 0, sizeof(mh));
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        errno = 0;
        BOOST_CHECK_EQUAL(::recvmsg(sp[0], &mh, MSG_DONTWAIT), -1);
        BOOST_CHECK_EQUAL(errno, EAGAIN);
    });

    // errno 保持：协程内无效 fd
    RunInCo([&]() {
        char buf[8];
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        struct msghdr mh;
        std::memset(&mh, 0, sizeof(mh));
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        errno = 0;
        BOOST_CHECK_EQUAL(::recvmsg(-1, &mh, 0), -1);
        BOOST_CHECK_EQUAL(errno, EBADF);
        errno = 0;
        BOOST_CHECK_EQUAL(::sendmsg(-1, &mh, 0), -1);
        BOOST_CHECK_EQUAL(errno, EBADF);
    });

    // 非协程直通
    {
        SocketPair sp;
        BOOST_REQUIRE(sp.Ok());
        const char msg[] = "direct-msg";
        struct iovec iov;
        iov.iov_base = const_cast<char*>(msg);
        iov.iov_len = sizeof(msg);
        struct msghdr mh;
        std::memset(&mh, 0, sizeof(mh));
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        BOOST_CHECK(::sendmsg(sp[1], &mh, 0) == (ssize_t)sizeof(msg));
        char buf[16] = {0};
        iov.iov_base = buf;
        BOOST_CHECK(::recvmsg(sp[0], &mh, 0) == (ssize_t)sizeof(msg));
        BOOST_TEST(std::string{buf} == std::string{msg});
    }
}

// readv/writev：两段 iovec；协程就绪立即返回、空 fd 挂起+延迟写唤醒、非协程直通、EBADF 保持
BOOST_AUTO_TEST_CASE(t_contract_readv_writev)
{
    RunInCo([&]() {
        SocketPair sp;
        BOOST_REQUIRE(sp.Ok());
        BOOST_REQUIRE(SetNonblock(sp[0]));
        BOOST_REQUIRE(::write(sp[1], "ABCD", 4) == 4);
        char b1 = 0, b2 = 0;
        struct iovec iov[2];
        iov[0].iov_base = &b1;
        iov[0].iov_len = 1;
        iov[1].iov_base = &b2;
        iov[1].iov_len = 1;
        BOOST_CHECK(::readv(sp[0], iov, 2) == 2);
        BOOST_TEST((b1 == 'A' && b2 == 'B'));
    });

    RunInCo([&]() {
        SocketPair sp;
        BOOST_REQUIRE(sp.Ok());
        BOOST_REQUIRE(SetNonblock(sp[1]));
        char m1 = 'X', m2 = 'Y';
        struct iovec iov[2];
        iov[0].iov_base = &m1;
        iov[0].iov_len = 1;
        iov[1].iov_base = &m2;
        iov[1].iov_len = 1;
        BOOST_CHECK(::writev(sp[1], iov, 2) == 2);
        char buf[8] = {0};
        BOOST_REQUIRE(::read(sp[0], buf, sizeof(buf)) == 2);
        BOOST_TEST((buf[0] == 'X' && buf[1] == 'Y'));
    });

    // 挂起等待：空 socketpair 上 readv 挂起，ticker 延迟原生写后唤醒
    std::atomic<int> ticker_writes{0};
    RunInCo([&]() {
        SocketPair sp;
        BOOST_REQUIRE(sp.Ok());
        BOOST_REQUIRE(SetNonblock(sp[0]));
        bbtco [&]() {
            bbtco_sleep(50);
            ++ticker_writes;
            ::write(sp[1], "wake", 4);
        };
        char buf[16] = {0};
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        BOOST_CHECK_EQUAL(::readv(sp[0], &iov, 1), 4);
        BOOST_TEST(std::string(buf, 4) == "wake");
    });
    BOOST_TEST(ticker_writes.load() == 1);

    // errno 保持：协程内无效 fd
    RunInCo([&]() {
        char buf[8];
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        errno = 0;
        BOOST_CHECK_EQUAL(::readv(-1, &iov, 1), -1);
        BOOST_CHECK_EQUAL(errno, EBADF);
        errno = 0;
        BOOST_CHECK_EQUAL(::writev(-1, &iov, 1), -1);
        BOOST_CHECK_EQUAL(errno, EBADF);
    });

    // 非协程直通
    {
        SocketPair sp;
        BOOST_REQUIRE(sp.Ok());
        char m1 = 'P', m2 = 'Q';
        struct iovec iov[2];
        iov[0].iov_base = &m1;
        iov[0].iov_len = 1;
        iov[1].iov_base = &m2;
        iov[1].iov_len = 1;
        BOOST_CHECK(::writev(sp[1], iov, 2) == 2);
        char buf[8] = {0};
        iov[0].iov_base = buf;
        iov[0].iov_len = 2;
        iov[1].iov_base = buf + 2;
        iov[1].iov_len = 4;
        BOOST_CHECK(::readv(sp[0], iov, 2) == 2);
        BOOST_TEST((buf[0] == 'P' && buf[1] == 'Q'));
    }
}

// accept4：抄 t_contract_accept 流程；新 fd 查 O_NONBLOCK 与 FD_CLOEXEC（SOCK_NONBLOCK|SOCK_CLOEXEC）
BOOST_AUTO_TEST_CASE(t_contract_accept4)
{
    FdGuard listen_fd{::socket(AF_INET, SOCK_STREAM, 0)};
    BOOST_REQUIRE(listen_fd.Get() >= 0);
    sockaddr_in addr;
    BOOST_REQUIRE(BindLoopbackPort0(listen_fd.Get(), &addr));
    BOOST_CHECK_EQUAL(::listen(listen_fd.Get(), 4), 0);

    std::atomic_bool client_done{false};
    RunInCo([&]() {
        // WHY 置非阻塞：同 t_contract_accept，否则原生 accept4 阻塞线程而非走挂起路径
        BOOST_REQUIRE(SetNonblock(listen_fd.Get()));
        bbtco [&]() {
            bbtco_sleep(50);
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd >= 0) {
                if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0)
                    client_done = true;
                ::close(fd);
            }
        };
        int cli = ::accept4(listen_fd.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        BOOST_REQUIRE(cli >= 0);
        BOOST_CHECK(IsNonblock(cli));  // 协程成功路径新 fd 带 O_NONBLOCK
        BOOST_CHECK((::fcntl(cli, F_GETFD, 0) & FD_CLOEXEC) != 0);  // SOCK_CLOEXEC 可测到
        ::close(cli);
        bbtco_sleep(50);  // 等 client 协程写定 client_done（同 t_contract_accept）
    });
    BOOST_TEST(client_done.load());

    // 非协程直通：独立阻塞 listener，backlog 先有连接再 accept4
    FdGuard blocky{::socket(AF_INET, SOCK_STREAM, 0)};
    BOOST_REQUIRE(blocky.Get() >= 0);
    sockaddr_in baddr;
    BOOST_REQUIRE(BindLoopbackPort0(blocky.Get(), &baddr));
    BOOST_CHECK_EQUAL(::listen(blocky.Get(), 4), 0);
    {
        FdGuard cli{::socket(AF_INET, SOCK_STREAM, 0)};
        BOOST_REQUIRE(cli.Get() >= 0);
        BOOST_CHECK_EQUAL(::connect(cli.Get(), (struct sockaddr*)&baddr, sizeof(baddr)), 0);
        int srv = ::accept4(blocky.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        BOOST_REQUIRE(srv >= 0);
        BOOST_CHECK(IsNonblock(srv));  // 直通路径同样按 flags 生效
        BOOST_CHECK((::fcntl(srv, F_GETFD, 0) & FD_CLOEXEC) != 0);
        ::close(srv);
        // 无效 fd errno 保持
        errno = 0;
        BOOST_CHECK_EQUAL(::accept4(-1, nullptr, nullptr, 0), -1);
        BOOST_CHECK_EQUAL(errno, EBADF);
    }
}

// EINTR：非协程空 pipe 阻塞 readv + EintrArm → -1/EINTR（在 t_contract_eintr 基础上补 readv 一例）
BOOST_AUTO_TEST_CASE(t_contract_readv_eintr)
{
    int fds[2];
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);
    FdGuard r{fds[0]};
    FdGuard w{fds[1]};
    {
        EintrArm arm{20000};  // 20ms 后 SIGALRM 打断阻塞 readv（非协程直通路径）
        char buf[8];
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        errno = 0;
        BOOST_CHECK_EQUAL(::readv(r.Get(), &iov, 1), -1);
        BOOST_CHECK_EQUAL(errno, EINTR);
    }
    const char msg[] = "ok";
    BOOST_REQUIRE(::write(w.Get(), msg, sizeof(msg)) == (ssize_t)sizeof(msg));
    char buf[8] = {0};
    BOOST_CHECK(::read(r.Get(), buf, sizeof(buf)) == (ssize_t)sizeof(msg));
}

// usleep：非协程 usleep(0) 直通立即 0；协程 usleep(100000)（100ms）期间 ticker 计数增长；
// 协程零时长立即 0（不复用 Hook_Sleep 的 ms<=0 → -1 语义，#229）
BOOST_AUTO_TEST_CASE(t_contract_usleep)
{
    {
        auto begin = std::chrono::steady_clock::now();
        BOOST_CHECK_EQUAL(::usleep(0), 0);
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count();
        BOOST_TEST(elapsed_ms < 50);
    }

    std::atomic<int> ticks{0};
    std::atomic_bool stop{false};
    RunInCo([&]() {
        bbtco [&]() {
            while (!stop.load()) {
                bbtco_sleep(10);
                ++ticks;
            }
        };
        BOOST_CHECK_EQUAL(::usleep(100000), 0);  // 协程 hook：YieldUntilTimeout(100ms)
        stop = true;
        bbtco_sleep(50);  // 等 ticker 退出，避免其写用例已析构的栈变量
    });
    BOOST_TEST(ticks.load() > 0);

    RunInCo([&]() {
        BOOST_CHECK_EQUAL(::usleep(0), 0);
    });
}

// nanosleep：非协程 {0,0} 立即 0；协程 50ms 期间 ticker 增长；req==NULL 非协程 -1/EINVAL
BOOST_AUTO_TEST_CASE(t_contract_nanosleep)
{
    {
        struct timespec req{0, 0}, rem;
        BOOST_CHECK_EQUAL(::nanosleep(&req, &rem), 0);
    }

    std::atomic<int> ticks{0};
    std::atomic_bool stop{false};
    RunInCo([&]() {
        bbtco [&]() {
            while (!stop.load()) {
                bbtco_sleep(10);
                ++ticks;
            }
        };
        struct timespec req{0, 50000000};  // 50ms
        BOOST_CHECK_EQUAL(::nanosleep(&req, nullptr), 0);
        stop = true;
        bbtco_sleep(50);
    });
    BOOST_TEST(ticks.load() > 0);

    // req==NULL：hook 直通原函数。实测 glibc 把 NULL 透传给 syscall → -1/EFAULT
    // （brief 猜的 EINVAL 不成立）；契约只保证 -1 + errno 非 0，不锁死具体值
    {
        errno = 0;
        BOOST_CHECK_EQUAL(::nanosleep(nullptr, nullptr), -1);
        BOOST_TEST(errno != 0);
    }
}

// clock_nanosleep：协程 CLOCK_MONOTONIC 相对 50ms + ticker；TIMER_ABSTIME 已到期立即 0；
// 非协程 {0,0} 直通立即 0
BOOST_AUTO_TEST_CASE(t_contract_clock_nanosleep)
{
    std::atomic<int> ticks{0};
    std::atomic_bool stop{false};
    RunInCo([&]() {
        bbtco [&]() {
            while (!stop.load()) {
                bbtco_sleep(10);
                ++ticks;
            }
        };
        struct timespec req{0, 50000000};
        BOOST_CHECK_EQUAL(::clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr), 0);
        stop = true;
        bbtco_sleep(50);
    });
    BOOST_TEST(ticks.load() > 0);

    // TIMER_ABSTIME 绝对时刻已过期（过去 1s）→ 立即返回 0
    RunInCo([&]() {
        struct timespec now, target;
        BOOST_REQUIRE_EQUAL(clock_gettime(CLOCK_MONOTONIC, &now), 0);
        target.tv_sec = now.tv_sec - 1;
        target.tv_nsec = now.tv_nsec;
        auto begin = std::chrono::steady_clock::now();
        BOOST_CHECK_EQUAL(::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, nullptr), 0);
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count();
        BOOST_TEST(elapsed_ms < 50);
    });

    {
        struct timespec req{0, 0};
        BOOST_CHECK_EQUAL(::clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr), 0);
    }
}

// EINTR：非协程 nanosleep({2,0}, &rem) 被 20ms SIGALRM 打断 → -1/EINTR（#229；
// 只测直通路径——协程定时器不会以 EINTR 提前结束）
BOOST_AUTO_TEST_CASE(t_contract_nanosleep_eintr)
{
    EintrArm arm{20000};
    struct timespec req{2, 0}, rem;
    errno = 0;
    int got = ::nanosleep(&req, &rem);
    BOOST_CHECK_EQUAL(got, -1);
    BOOST_CHECK_EQUAL(errno, EINTR);
    // rem 只作弱断言：真睡满 2s 说明 EINTR 语义没生效
    BOOST_TEST(rem.tv_sec < 2);
}

BOOST_AUTO_TEST_CASE(test_env_unload)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
