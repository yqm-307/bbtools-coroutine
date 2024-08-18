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
        // bbtco [this](){
            _Run();
            m_cdl.Down();
        // };

        m_cdl.Wait();
    }

protected:
    void _Run(){
        int listen_fd = bbt::net::Util::CreateListen("", m_listen_port, false);
        // int listen_fd = bbt::net::Util::CreateListen("", m_listen_port, true);
        Assert(listen_fd >= 0);

        sockaddr_in cli_addr;
        socklen_t len = sizeof(cli_addr);

        while (1)
        {
            char buf[2];
            int new_fd = ::accept(listen_fd, (sockaddr *)(&cli_addr), &len);
            // bbtco [new_fd](){
            Assert(new_fd >= 0);

            // Assert(bbt::net::Util::SetFdNoBlock(new_fd) == 0);

            bool noclose = true;
            while (noclose) {
                int read_len = ::read(new_fd, buf, 2);

                if (read_len == 0)
                    noclose = false;
                
                int write_len = ::write(new_fd, buf, read_len);
                printf("echo: %s\n", buf);
                Assert(read_len == write_len);


            }

            ::close(new_fd);
            printf("echo done!\n");
            // };
            fflush(stdout);
        }
        ::close(listen_fd);
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