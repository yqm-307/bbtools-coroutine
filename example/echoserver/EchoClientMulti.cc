#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <bbt/base/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

const char msg[] = "1";

class Client
{
public:
    Client(std::string ip, short port, int round_num):
        m_ip(ip), m_port(port), m_cdl(round_num), m_round_num(round_num){}
    ~Client(){}

    void Start() {
        for (int i = 0; i < m_round_num; ++i) {
            bbtco [this, i](){
                _Run(i);
                printf("end do once: %d\n", i);
                m_cdl.Down();
            };
        }
        m_cdl.Wait();
    }

protected:
    void _Run(int i) {
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
        ret = ::write(fd, std::to_string(i).c_str(), 1);
        Assert(ret != -1);

        ret = ::read(fd, read_buf, sizeof(read_buf));
        Assert(ret != -1);
        Assert(ret == 1);
        ::close(fd);
    }
private:
    bbt::thread::CountDownLatch m_cdl{1};
    int m_round_num;
    std::string m_ip;
    short       m_port;
};

int main()
{
    g_scheduler->Start(true);
    Client c{"127.0.0.1", 10010, 1000};
    c.Start();

    g_scheduler->Stop();
}