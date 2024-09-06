#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <bbt/base/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

class Client
{
public:
    Client(std::string ip, short port):
        m_ip(ip), m_port(port), m_cdl(1){}
    ~Client(){}

    void Start() {
        bbtco [this](){
            _Run();
            printf("success\n");
            m_cdl.Down();
        };
        m_cdl.Wait();
    }

protected:
    void _Run() {
        sockaddr_in addr;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(10010);
        addr.sin_family = AF_INET;
        char read_buf[32];
        memset(read_buf, '\0', sizeof(read_buf));

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        Assert(fd >= 0);
        int ret = ::connect(fd, (sockaddr *)(&addr), sizeof(addr));
        Assert(ret == 0);
        for (int i = 0; i<10000; ++i) {
            std::string msg = std::to_string(i);
            ret = ::write(fd, msg.c_str(), sizeof(msg));
            printf("write\n");
            Assert(ret != -1);

            ret = ::read(fd, read_buf, sizeof(read_buf));
            printf("echo: %s\n", read_buf);
            Assert(ret != -1);
            Assert(ret == sizeof(msg));
        }

        ::close(fd);
    }
private:
    bbt::thread::CountDownLatch m_cdl{1};
    std::string m_ip;
    short       m_port;
};

int main()
{
    g_scheduler->Start();
    Client c{"127.0.0.1", 10010};
    c.Start();

    g_scheduler->Stop();
}