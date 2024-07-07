#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/base/thread/Lock.hpp>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

BOOST_AUTO_TEST_SUITE(HookSystemFunc)

BOOST_AUTO_TEST_CASE(t_hook_call)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    ::close(fd);
}

BOOST_AUTO_TEST_CASE(t_hook_socket)
{
    g_scheduler->Start(true);

    bbt::thread::CountDownLatch l{1};

    bbtco [&l](){
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);

        int status = ::fcntl(fd, F_GETFL);
        BOOST_CHECK_GT(status, 0);
        BOOST_CHECK(status & O_NONBLOCK);
        l.Down();
    };

    l.Wait();
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_CASE(t_hook_connect)
{
    g_scheduler->Start(true);

    bbt::thread::CountDownLatch l{1};

    bbtco [&l](){
        sockaddr_in addr;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(22);
        addr.sin_family = AF_INET;

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_ASSERT(fd >= 0);
        int ret = ::connect(fd, (sockaddr*)(&addr), sizeof(addr));
        BOOST_CHECK_MESSAGE(ret == 0, "errno=" << errno << "\tret=" << ret << "\tfd=" << fd);
        l.Down();
    };

    l.Wait();

    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()