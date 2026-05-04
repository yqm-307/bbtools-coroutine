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

namespace
{

sockaddr_in LoopbackAddr(int port)
{
    sockaddr_in addr{};
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    addr.sin_family = AF_INET;
    return addr;
}

int GetBoundPort(int fd)
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    Assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    return ntohs(addr.sin_port);
}

}

BOOST_AUTO_TEST_SUITE(HookSystemFunc)

BOOST_AUTO_TEST_CASE(test_env_setup)
{
    g_scheduler->Start();
}

BOOST_AUTO_TEST_CASE(t_hook_call)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int status = ::fcntl(fd, F_GETFL);

    BOOST_REQUIRE_GE(status, 0);
    BOOST_CHECK_EQUAL(status & O_NONBLOCK, 0);
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
    bbt::core::thread::CountDownLatch l{2};
    auto chan = bbt::coroutine::Chan<int, 1>();

    bbtco[&l, chan]()
    {
        auto rlt = bbt::core::net::CreateListen("", 0, true);
        if (rlt.IsErr())
        {
            BOOST_TEST_MESSAGE("[server] create listen failed, errno=" << rlt.Err().What());
            BOOST_FAIL("create listen failed");
        }

        int fd = rlt.Ok();
        chan << GetBoundPort(fd);
        BOOST_ASSERT(fd >= 0);
        sockaddr_in cli_addr;
        socklen_t len = sizeof(cli_addr);
        int new_fd = ::accept(fd, (sockaddr *)(&cli_addr), &len);
        BOOST_REQUIRE_GE(new_fd, 0);
        ::close(new_fd);
        ::close(fd);
        l.Down();
    };

    bbtco[&l, chan]()
    {
        int port = 0;
        chan >> port;
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_ASSERT(fd >= 0);
        auto addr = LoopbackAddr(port);
        int ret = ::connect(fd, (sockaddr *)(&addr), sizeof(addr));
        BOOST_CHECK_MESSAGE(ret == 0, "errno=" << errno << "\tret=" << ret << "\tfd=" << fd);
        ::close(fd);
        l.Down();
    };

    l.Wait();
}

const char *msg = "hello world";

BOOST_AUTO_TEST_CASE(t_hook_write)
{
    bbt::core::thread::CountDownLatch l{2};
    auto chan = bbt::coroutine::Chan<int, 1>();

    bbtco[&l, chan]()
    {
        BOOST_TEST_MESSAGE("[server] server co=" << bbt::coroutine::GetLocalCoroutineId());
        auto rlt = bbt::core::net::CreateListen("", 0, true);
        if (rlt.IsErr())
        {
            BOOST_TEST_MESSAGE("[server] create listen failed, errno=" << rlt.Err().What());
            BOOST_FAIL("create listen failed");
        }

        int fd = rlt.Ok();
        chan << GetBoundPort(fd);
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

    bbtco[&l, chan]()
    {
        int port = 0;
        chan >> port;
        BOOST_TEST_MESSAGE("[client] client co=" << bbt::coroutine::GetLocalCoroutineId());
        auto addr = LoopbackAddr(port);

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_ASSERT(fd >= 0);
        BOOST_TEST_MESSAGE("[client] create socket succ!");
        int ret = ::connect(fd, (sockaddr *)(&addr), sizeof(addr));
        BOOST_CHECK_MESSAGE(ret == 0, "[connect] errno=" << errno << "\tret=" << ret << "\tfd=" << fd);
        BOOST_ASSERT(ret == 0);
        BOOST_TEST_MESSAGE("[client] connect succ");
        bbtco_sleep(100);
        ret = ::write(fd, msg, strlen(msg));
        BOOST_CHECK_MESSAGE(ret != -1, "[write] errno=" << errno << "\tret=" << ret << "\tfd=" << fd);
        BOOST_ASSERT(ret != -1);
        BOOST_TEST_MESSAGE("[client] send succ");
        ::close(fd);
        l.Down();
        BOOST_TEST_MESSAGE("[client] exit!");
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_hook_send)
{
    bbt::core::thread::CountDownLatch l{2};
    auto chan = bbt::coroutine::Chan<int, 1>();

    bbtco[&l, chan]()
    {
        auto rlt = bbt::core::net::CreateListen("", 0, true);
        if (rlt.IsErr())
        {
            BOOST_TEST_MESSAGE("[server] create listen failed, errno=" << rlt.Err().What());
            BOOST_FAIL("create listen failed");
        }
        int fd = rlt.Ok();
        chan << GetBoundPort(fd);
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

    bbtco[&l, chan]()
    {
        int port = 0;
        chan >> port;
        auto addr = LoopbackAddr(port);

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_ASSERT(fd >= 0);
        int ret = ::connect(fd, (sockaddr *)(&addr), sizeof(addr));
        BOOST_CHECK_MESSAGE(ret == 0, "[connect] errno=" << errno << "\tret=" << ret << "\tfd=" << fd);
        bbtco_sleep(100);
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

BOOST_AUTO_TEST_CASE(t_bbtco_wait_for_timeout)
{
    bbt::core::thread::CountDownLatch l{1};
    int pipefd[2];
    BOOST_REQUIRE(::pipe(pipefd) == 0);

    bbtco_ref {
        auto begin = bbt::core::clock::gettime();
        bbtco_wait_for(pipefd[0], bbtco_emev_readable | bbtco_emev_timeout, 50);
        auto end = bbt::core::clock::gettime();
        BOOST_CHECK_GE(end - begin, 50);
        l.Down();
    };

    l.Wait();
    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

BOOST_AUTO_TEST_CASE(t_hook_sleep_timeout)
{
    bbt::core::thread::CountDownLatch l{1};
    int elapsed = 0;

    bbtco[&l, &elapsed]() {
        auto begin = bbt::core::clock::gettime();
        BOOST_REQUIRE_EQUAL(bbt::coroutine::detail::Hook_Sleep(50), 0);
        elapsed = bbt::core::clock::gettime() - begin;
        l.Down();
    };

    l.Wait();
    BOOST_CHECK_GE(elapsed, 50);
}

BOOST_AUTO_TEST_CASE(test_env_unload)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
