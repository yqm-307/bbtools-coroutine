#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <bbt/base/net/SocketUtil.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/base/clock/Clock.hpp>
using namespace bbt::coroutine;

#define print(msg) (std::cout << msg << std::endl)

const char *msg = "hello world";

void test()
{
    bbt::thread::CountDownLatch l{2};

    bbtco[&l]()
    {
        print("[server] server co=" << bbt::coroutine::GetLocalCoroutineId());
        int fd = bbt::net::Util::CreateListen("", 10001, true);
        Assert(fd >= 0);
        print("[server] create succ listen fd=" << fd);
        sockaddr_in cli_addr;
        char *buf = new char[1024];
        memset(buf, '\0', 1024);
        socklen_t len = sizeof(cli_addr);

        print("[server] accept start!");
        int new_fd = ::accept(fd, (sockaddr *)(&cli_addr), &len);
        Assert(new_fd >= 0);

        print("[server] read msg! fd=" << new_fd);
        int read_len = ::read(new_fd, buf, 1024);
        Assert(read_len != 0);

        Assert(std::string{msg} == std::string{buf});
        print("[server] recv" << std::string{buf});

        print("[server] exit!");
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
        Assert(fd >= 0);
        print("[client] connect start! fd=" << fd);
        int ret = ::connect(fd, (sockaddr *)(&addr), sizeof(addr));
        Assert(ret == 0);
        // BOOST_CHECK_MESSAGE(ret == 0, "[connect] errno=" << errno << "\tret=" << ret << "\tfd=" << fd);
        print("[client] send msg to server! fd=" << fd);
        ret = ::write(fd, msg, strlen(msg));
        // BOOST_CHECK_MESSAGE(ret != -1, "[write] errno=" << errno << "\tret=" << ret << "\tfd=" << fd);
        Assert(ret != -1);
        ::sleep(1);

        print("[client] exit!");
        ::close(fd);
        l.Down();
    };

    l.Wait();
}

int main()
{
    g_scheduler->Start(true);
    for (int i = 0;;i++) {
        printf("================== turn %d ==============\n", i);
        test();
    }

    g_scheduler->Stop();
}