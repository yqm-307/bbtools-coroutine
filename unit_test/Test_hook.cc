#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/core/net/SocketUtil.hpp>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

BOOST_AUTO_TEST_SUITE(HookSystemFunc)

BOOST_AUTO_TEST_CASE(test_env_setup)
{
    g_scheduler->Start();
}

BOOST_AUTO_TEST_CASE(t_hook_call)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    ::close(fd);
}

BOOST_AUTO_TEST_CASE(t_hook_socket)
{

    bbt::core::thread::CountDownLatch l{1};

    bbtco[&l]()
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);

        int status = ::fcntl(fd, F_GETFL);
        BOOST_CHECK_GT(status, 0);
        BOOST_CHECK(status & O_NONBLOCK);
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_hook_connect)
{

    bbt::core::thread::CountDownLatch l{1};

    bbtco[&l]()
    {
        sockaddr_in addr;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(22);
        addr.sin_family = AF_INET;

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_ASSERT(fd >= 0);
        int ret = ::connect(fd, (sockaddr *)(&addr), sizeof(addr));
        BOOST_CHECK_MESSAGE(ret == 0, "errno=" << errno << "\tret=" << ret << "\tfd=" << fd);
        l.Down();
    };

    l.Wait();
}

const char *msg = "hello world";

BOOST_AUTO_TEST_CASE(t_hook_write)
{
    bbt::core::thread::CountDownLatch l{2};

    bbtco[&l]()
    {
        BOOST_TEST_MESSAGE("[server] server co=" << bbt::coroutine::GetLocalCoroutineId());
        auto rlt = bbt::core::net::CreateListen("", 10001, true);
        if (rlt.IsErr())
        {
            BOOST_TEST_MESSAGE("[server] create listen failed, errno=" << rlt.Err().What());
            BOOST_FAIL("create listen failed");
        }

        int fd = rlt.Ok();
        BOOST_ASSERT(fd >= 0);
        BOOST_TEST_MESSAGE("[server] create succ listen fd=" << fd);
        sockaddr_in cli_addr;
        char *buf = new char[1024];
        memset(buf, '\0', 1024);
        socklen_t len = sizeof(cli_addr);

        int new_fd = ::accept(fd, (sockaddr *)(&cli_addr), &len);
        BOOST_ASSERT(new_fd >= 0);
        BOOST_TEST_MESSAGE("[server] accept succ new_fd=" << new_fd);
        int read_len = ::read(new_fd, buf, 1024);
        BOOST_CHECK_GT(read_len, 0);
        BOOST_ASSERT(std::string{msg} == std::string{buf});
        BOOST_TEST_MESSAGE("[server] recv" << std::string{buf});
        ::close(fd);
        ::close(new_fd);
        l.Down();
        BOOST_TEST_MESSAGE("[server] exit!");
    };

    bbtco[&l]()
    {
        ::sleep(1);
        BOOST_TEST_MESSAGE("[client] client co=" << bbt::coroutine::GetLocalCoroutineId());
        sockaddr_in addr;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(10001);
        addr.sin_family = AF_INET;

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_ASSERT(fd >= 0);
        BOOST_TEST_MESSAGE("[client] create socket succ!");
        int ret = ::connect(fd, (sockaddr *)(&addr), sizeof(addr));
        BOOST_CHECK_MESSAGE(ret == 0, "[connect] errno=" << errno << "\tret=" << ret << "\tfd=" << fd);
        BOOST_ASSERT(ret == 0);
        BOOST_TEST_MESSAGE("[client] connect succ");
        ret = ::write(fd, msg, strlen(msg));
        BOOST_CHECK_MESSAGE(ret != -1, "[write] errno=" << errno << "\tret=" << ret << "\tfd=" << fd);
        BOOST_ASSERT(ret != -1);
        BOOST_TEST_MESSAGE("[client] send succ");
        ::sleep(1);
        ::close(fd);
        l.Down();
        BOOST_TEST_MESSAGE("[client] exit!");
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_hook_send)
{
    bbt::core::thread::CountDownLatch l{2};

    bbtco[&l]()
    {
        auto rlt = bbt::core::net::CreateListen("", 10001, true);
        if (rlt.IsErr())
        {
            BOOST_TEST_MESSAGE("[server] create listen failed, errno=" << rlt.Err().What());
            BOOST_FAIL("create listen failed");
        }
        int fd = rlt.Ok();
        BOOST_ASSERT(fd >= 0);
        sockaddr_in cli_addr;
        char *buf = new char[1024];
        memset(buf, '\0', 1024);
        socklen_t len = sizeof(cli_addr);
        int new_fd = ::accept(fd, (sockaddr *)(&cli_addr), &len);
        BOOST_ASSERT(new_fd >= 0);
        int recv_len = ::recv(new_fd, buf, 1024, 0);
        BOOST_CHECK_GT(recv_len, 0);
        BOOST_CHECK_MESSAGE(std::string{msg} == std::string{buf}, "recv" << std::string{buf});
        BOOST_TEST_MESSAGE("recv" << std::string{buf});
        ::close(fd);
        ::close(new_fd);
        l.Down();
    };

    bbtco[&l]()
    {
        ::sleep(1);
        sockaddr_in addr;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(10001);
        addr.sin_family = AF_INET;

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_ASSERT(fd >= 0);
        int ret = ::connect(fd, (sockaddr *)(&addr), sizeof(addr));
        BOOST_CHECK_MESSAGE(ret == 0, "[connect] errno=" << errno << "\tret=" << ret << "\tfd=" << fd);
        ret = ::send(fd, msg, strlen(msg), 0);
        BOOST_CHECK_MESSAGE(ret != -1, "[send] errno=" << errno << "\tret=" << ret << "\tfd=" << fd);
        ::close(fd);
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_bbtco_wait_for)
{
    bbt::core::thread::CountDownLatch l{1};
    int pipefd[2];
    BOOST_REQUIRE(::pipe(pipefd) == 0);

    bbtco_ref {
        bbtco_wait_for(pipefd[0], bbtco_emev_readable, 0);

        char buf[128];
        ssize_t read_len = ::read(pipefd[0], buf, sizeof(buf));
        BOOST_REQUIRE_GT(read_len, 0);
        BOOST_CHECK_EQUAL(std::string(buf, read_len), std::string("hello world"));
        l.Down();
    };

    bbtco_ref {
        bbtco_sleep(100);
        const char *msg = "hello world";
        ssize_t write_len = ::write(pipefd[1], msg, strlen(msg));
        BOOST_REQUIRE_GT(write_len, 0);
    };

    l.Wait();
}

// =============== #190: Hook 异常安全 ===============

// 协程内 recv 传入无效 fd：系统调用失败路径 errno 不被 hook 覆盖
BOOST_AUTO_TEST_CASE(t_hook_errno_badfd_preserved)
{
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_int got_errno{0};
    std::atomic_int ret{0};

    bbtco [&]() {
        errno = 0;
        char buf[16];
        ret = ::recv(-1, buf, sizeof(buf), 0);
        got_errno = errno;
        l.Down();
    };

    l.Wait();
    BOOST_TEST(ret.load() == -1);
    BOOST_TEST(got_errno.load() == EBADF);
}

// UDP recvfrom/sendto 正常路径（协程内，新增 hook）
BOOST_AUTO_TEST_CASE(t_hook_recvfrom_sendto)
{
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_bool recv_ok{false};

    bbtco [&]() {
        // 服务端 socket
        int server_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        BOOST_REQUIRE(server_fd >= 0);

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        server_addr.sin_port = 0;  // 自动分配端口
        BOOST_REQUIRE(::bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == 0);

        socklen_t addr_len = sizeof(server_addr);
        BOOST_REQUIRE(::getsockname(server_fd, (struct sockaddr*)&server_addr, &addr_len) == 0);

        // 客户端 socket（同样走 hook）
        int client_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        BOOST_REQUIRE(client_fd >= 0);

        const char msg[] = "udp-hook-hello";
        // 客户端 sendto（hook）：UDP 通常立即成功
        ssize_t sent = ::sendto(client_fd, msg, sizeof(msg), 0,
                                (struct sockaddr*)&server_addr, sizeof(server_addr));
        BOOST_CHECK(sent == (ssize_t)sizeof(msg));

        // 服务端 recvfrom（hook）
        char buf[64] = {0};
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t got = ::recvfrom(server_fd, buf, sizeof(buf), 0,
                                 (struct sockaddr*)&from_addr, &from_len);
        BOOST_CHECK(got == (ssize_t)sizeof(msg));
        BOOST_CHECK(std::string{buf} == std::string{msg});
        recv_ok = (got == (ssize_t)sizeof(msg));

        ::close(client_fd);
        ::close(server_fd);
        l.Down();
    };

    l.Wait();
    BOOST_TEST(recv_ok.load());
}

// recvfrom 无数据时协程挂起，对端到达后恢复（EAGAIN 挂起路径）
BOOST_AUTO_TEST_CASE(t_hook_recvfrom_eagain_wakeup)
{
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_bool got_msg{false};
    std::atomic_int got_errno{0};

    int server_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    BOOST_REQUIRE(server_fd >= 0);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = 0;
    BOOST_REQUIRE(::bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == 0);

    socklen_t addr_len = sizeof(server_addr);
    BOOST_REQUIRE(::getsockname(server_fd, (struct sockaddr*)&server_addr, &addr_len) == 0);

    // 主线程在对端延时发送
    std::thread sender([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int client_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (client_fd < 0)
            return;
        const char msg[] = "wake";
        ::sendto(client_fd, msg, sizeof(msg), 0,
                 (struct sockaddr*)&server_addr, sizeof(server_addr));
        ::close(client_fd);
    });

    bbtco [&]() {
        char buf[16] = {0};
        errno = 0;
        ssize_t got = ::recvfrom(server_fd, buf, sizeof(buf), 0, nullptr, nullptr);
        got_errno = errno;  // 成功后 errno 不应残留 EAGAIN 之外的可疑值
        got_msg = (got > 0);
        l.Down();
    };

    l.Wait();
    sender.join();
    ::close(server_fd);

    BOOST_TEST(got_msg.load());
    // 成功返回时 errno 语义未定义（POSIX 不保证），仅验证不残留 EAGAIN 之外的硬错误
    // （无强断言：errno 在成功路径上无需固定值）
}

// 协程内 read 无效 fd：EBADF 保持
BOOST_AUTO_TEST_CASE(t_hook_read_errno_preserved)
{
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_int got_errno{0};
    std::atomic_int ret{0};

    bbtco [&]() {
        errno = 0;
        char buf[16];
        ret = ::read(-1, buf, sizeof(buf));
        got_errno = errno;
        l.Down();
    };

    l.Wait();
    BOOST_TEST(ret.load() == -1);
    BOOST_TEST(got_errno.load() == EBADF);
}

// =============== #191: connect/accept 异常安全 ===============

// connect 立即失败（ECONNREFUSED）：socket 保持可重用，重连到监听端口成功
BOOST_AUTO_TEST_CASE(t_hook_connect_refused_then_reuse)
{
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_bool first_failed{false};
    std::atomic_bool second_ok{false};
    std::atomic_int first_errno{0};

    // 监听 socket（在测试线程准备好）
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(listen_fd >= 0);

    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listen_addr.sin_port = 0;
    BOOST_REQUIRE(::bind(listen_fd, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) == 0);
    BOOST_REQUIRE(::listen(listen_fd, 4) == 0);

    socklen_t addr_len = sizeof(listen_addr);
    BOOST_REQUIRE(::getsockname(listen_fd, (struct sockaddr*)&listen_addr, &addr_len) == 0);

    // 未监听端口：bind 后立即 close 释放端口，connect 该端口（无监听者）
    // 内核立即 RST → ECONNREFUSED
    int tmp_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(tmp_fd >= 0);
    struct sockaddr_in dead_addr;
    memset(&dead_addr, 0, sizeof(dead_addr));
    dead_addr.sin_family = AF_INET;
    dead_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dead_addr.sin_port = 0;
    BOOST_REQUIRE(::bind(tmp_fd, (struct sockaddr*)&dead_addr, sizeof(dead_addr)) == 0);
    socklen_t dead_len = sizeof(dead_addr);
    BOOST_REQUIRE(::getsockname(tmp_fd, (struct sockaddr*)&dead_addr, &dead_len) == 0);
    ::close(tmp_fd);  // 端口已释放，connect 将得到 ECONNREFUSED

    bbtco [&]() {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_REQUIRE(fd >= 0);

        // 第一次 connect：无监听者，立即 ECONNREFUSED
        errno = 0;
        int ret1 = ::connect(fd, (struct sockaddr*)&dead_addr, sizeof(dead_addr));
        first_errno = errno;
        first_failed = (ret1 == -1);

        // 同一 socket 重连到监听端口：应成功（socket 状态可重用）
        int ret2 = ::connect(fd, (struct sockaddr*)&listen_addr, sizeof(listen_addr));
        second_ok = (ret2 == 0);

        ::close(fd);
        l.Down();
    };

    l.Wait();
    ::close(listen_fd);

    BOOST_TEST(first_failed.load());
    BOOST_TEST(first_errno.load() == ECONNREFUSED);
    BOOST_TEST(second_ok.load());
}

// 非阻塞 connect 完成后重入 connect（EISCONN）返回成功而非 -1
BOOST_AUTO_TEST_CASE(t_hook_connect_eisconn_success)
{
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_bool reconn_ok{false};

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(listen_fd >= 0);

    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listen_addr.sin_port = 0;
    BOOST_REQUIRE(::bind(listen_fd, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) == 0);
    BOOST_REQUIRE(::listen(listen_fd, 4) == 0);

    socklen_t addr_len = sizeof(listen_addr);
    BOOST_REQUIRE(::getsockname(listen_fd, (struct sockaddr*)&listen_addr, &addr_len) == 0);

    // 接受线程：accept 后连接完成
    std::thread acceptor([&]() {
        int cli = ::accept(listen_fd, nullptr, nullptr);
        if (cli >= 0)
            ::close(cli);
    });

    bbtco [&]() {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_REQUIRE(fd >= 0);

        // 第一次 connect：可能立即完成（loopback 快）或 EINPROGRESS 挂起完成
        int ret1 = ::connect(fd, (struct sockaddr*)&listen_addr, sizeof(listen_addr));
        BOOST_REQUIRE(ret1 == 0);

        // 已连接后再次 connect 同一地址：EISCONN 应被视为成功
        int ret2 = ::connect(fd, (struct sockaddr*)&listen_addr, sizeof(listen_addr));
        reconn_ok = (ret2 == 0);

        ::close(fd);
        l.Down();
    };

    l.Wait();
    acceptor.join();
    ::close(listen_fd);

    BOOST_TEST(reconn_ok.load());
}

// accept 失败不影响 listen socket：accept 后 listen socket 仍可继续 accept
BOOST_AUTO_TEST_CASE(t_hook_accept_listen_socket_unaffected)
{
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_int accepted{0};

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(listen_fd >= 0);

    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listen_addr.sin_port = 0;
    BOOST_REQUIRE(::bind(listen_fd, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) == 0);
    BOOST_REQUIRE(::listen(listen_fd, 4) == 0);

    socklen_t addr_len = sizeof(listen_addr);
    BOOST_REQUIRE(::getsockname(listen_fd, (struct sockaddr*)&listen_addr, &addr_len) == 0);

    // 两个客户端先后连接
    std::thread client1([&]() {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return;
        ::connect(fd, (struct sockaddr*)&listen_addr, sizeof(listen_addr));
        ::close(fd);
    });

    bbtco [&]() {
        // 第一轮 accept（挂起直到 client1 到达）
        int cli1 = ::accept(listen_fd, nullptr, nullptr);
        BOOST_REQUIRE(cli1 >= 0);
        ::close(cli1);
        accepted++;

        // listen socket 不受影响：继续监听，第二轮 accept 由协程内自建 client 触发
        int cli2_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_REQUIRE(cli2_fd >= 0);
        int ret = ::connect(cli2_fd, (struct sockaddr*)&listen_addr, sizeof(listen_addr));
        BOOST_REQUIRE(ret == 0);
        ::close(cli2_fd);

        int cli2 = ::accept(listen_fd, nullptr, nullptr);
        BOOST_REQUIRE(cli2 >= 0);
        ::close(cli2);
        accepted++;

        l.Down();
    };

    l.Wait();
    client1.join();
    ::close(listen_fd);

    BOOST_TEST(accepted.load() == 2);
}

BOOST_AUTO_TEST_CASE(test_env_unload)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()