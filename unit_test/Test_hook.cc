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
        int fd = bbt::core::net::Util::CreateListen("", 10001, true);
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
        int fd = bbt::core::net::Util::CreateListen("", 10001, true);
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

BOOST_AUTO_TEST_CASE(test_env_unload)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()