#include <iostream>
#include <bbt/core/net/SocketUtil.hpp>
#include <bbt/core/thread/Lock.hpp>
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
        auto rlt = bbt::core::net::CreateListen("127.0.0.1", m_listen_port, false);
        if (rlt.IsErr())
        {
            std::cerr << "CreateListen failed: " << rlt.Err().What() << std::endl;
            return;
        }
        int listen_fd = rlt.Ok();
        Assert(listen_fd >= 0);

        sockaddr_in cli_addr;
        socklen_t len = sizeof(cli_addr);

        while (1)
        {
            int new_fd = ::accept(listen_fd, (sockaddr *)(&cli_addr), &len);
            bbtco [new_fd](){
                char buf[32];
                Assert(new_fd >= 0);

                // Assert(bbt::net::Util::SetFdNoBlock(new_fd) == 0);

                bool close = false;
                while (!close) {
                    memset(buf, '\0', sizeof(buf));

                    int read_len = ::read(new_fd, buf, sizeof(buf));
                    if (read_len <= 0)
                    {
                        close = true;
                        break;
                    }

                    int write_len = ::write(new_fd, buf, read_len);

                    if (read_len <= 0 or write_len <= 0)
                        close = true;

                    // printf("echo: %s\n", buf);
                }

                ::close(new_fd);
                printf("echo done!\n");
                fflush(stdout);
            };
        }
        ::close(listen_fd);
    }
private:
    bbt::core::thread::CountDownLatch m_cdl{1};
    short m_listen_port{-1};
};

int main()
{
    g_scheduler->Start();
    Server s{10010};
    s.Start();

    g_scheduler->Stop();
}