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

BOOST_AUTO_TEST_CASE(test_env_unload)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()