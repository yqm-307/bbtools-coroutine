#include <iostream>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

/* 最小本地 listen helper：替代 bbt::core::net::CreateListen。
 * net 模块归 bbtools-infra，coroutine 不依赖；失败返回 -1 并保留 errno。 */
static int CreateListenTcp(const char* ip, short port, bool noblock)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int one = 1;
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (ip == nullptr || strlen(ip) == 0)
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else
        ::inet_pton(AF_INET, ip, &addr.sin_addr.s_addr);

    bool failed =
        (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) ||
        (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) ||
        (::listen(fd, SOMAXCONN) < 0);

    if (!failed && noblock) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        failed = (flags < 0) || (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0);
    }

    if (failed) {
        int e = errno;
        ::close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

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
        int listen_fd = CreateListenTcp("127.0.0.1", m_listen_port, false);
        if (listen_fd < 0)
        {
            std::cerr << "CreateListen failed, errno=" << errno << std::endl;
            return;
        }
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

                    int write_len = 0;
                    while (write_len < read_len) {
                        const int n = ::send(new_fd, buf + write_len,
                                             read_len - write_len, MSG_NOSIGNAL);
                        if (n <= 0) {
                            close = true;
                            break;
                        }
                        write_len += n;
                    }

                    if (read_len <= 0 or write_len != read_len)
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
    std::signal(SIGPIPE, SIG_IGN);
    g_scheduler->Start();
    Server s{10010};
    s.Start();

    g_scheduler->Stop();
}