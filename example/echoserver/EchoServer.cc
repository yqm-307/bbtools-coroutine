#include <iostream>
#include <bbt/base/net/SocketUtil.hpp>
#include <bbt/base/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

class Server
{
public:
    Server(short listen_port):
        m_listen_port(listen_port){}
    ~Server(){}

    void Start(){
        bbtco [this](){
            _Run();
            m_cdl.Down();
        };

        m_cdl.Wait();
    }

protected:
    void _Run(){
        int listen_fd = bbt::net::Util::CreateListen("", m_listen_port, true);
        Assert(listen_fd >= 0);

        sockaddr_in cli_addr;
        char *buf = new char[1024];
        socklen_t len = sizeof(cli_addr);

        while (1)
        {
            int new_fd = ::accept(listen_fd, (sockaddr *)(&cli_addr), &len);
            Assert(new_fd >= 0);
            memset(buf, '\0', 1024);

            Assert(bbt::net::Util::SetFdNoBlock(new_fd) == 0);

            printf("[server] read msg! fd=%d\n", new_fd);

            int read_len = ::read(new_fd, buf, 1024);
            Assert(read_len > 0);
            printf("[server] read msg succ! fd=%d msg=%s\n", new_fd, buf);
            int write_len = ::write(new_fd, buf, read_len);
            Assert(read_len == write_len);
            ::close(new_fd);
        }

        ::close(listen_fd);
        delete[] buf;
    }
private:
    bbt::thread::CountDownLatch m_cdl{1};
    short m_listen_port{-1};
};

int main()
{
    g_scheduler->Start(true);
    Server s{10010};
    s.Start();

    g_scheduler->Stop();
}