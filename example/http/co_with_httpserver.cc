#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <functional>
#include <string>
#include <stdexcept>

#include <bbt/http/detail/ReqParser.hpp>
#include <bbt/coroutine/coroutine.hpp>

const char* response_str = 
"HTTP/1.1 200 OK\r\n"
"Content-Type: text/plain\r\n"
"Content-Length: 42\r\n"
"\r\n"
"Welcome to BBT Coroutine HTTP Server!\r\n";

class HttpServer
{
public:

    HttpServer(const std::string& addr, int port)
        : m_listen_addr(addr), m_listen_port(port)
    {
        // 创建监听套接字
        m_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (m_listen_fd < 0) {
            throw std::runtime_error("Failed to create socket");
        }

        // 设置地址和端口
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = inet_addr(m_listen_addr.c_str());
        server_addr.sin_port = htons(m_listen_port);

        // 绑定套接字
        if (bind(m_listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            close(m_listen_fd);
            throw std::runtime_error("Failed to bind socket");
        }
    }

    ~HttpServer()
    {
        if (m_listen_fd != -1) {
            ::close(m_listen_fd);
        }
    }

    // 启动服务器
    void Run()
    {
        // 开始监听
        if (listen(m_listen_fd, 10) < 0) {
            close(m_listen_fd);
            throw std::runtime_error("Failed to listen on socket");
        }

        while (m_is_running)
        {
            // 接受连接
            int client_fd = accept(m_listen_fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue; // 被信号中断，继续循环
                }

                close(m_listen_fd);
                throw std::runtime_error("Failed to accept connection");
            }

            bbtco [this, client_fd](){
                CoHttpProcesser(client_fd); // 启动协程处理请求
            };
        }

        close(m_listen_fd);
        m_listen_fd = -1; // 确保套接字被关闭
    }

    void Stop()
    {
        m_is_running = false;
    }

    void CoHttpProcesser(int fd)
    {
        // 设置非阻塞
        if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
            close(fd);
            throw std::runtime_error("Failed to set non-blocking mode");
        }

        bbt::http::detail::ReqParser parser;
        char buffer[4096];

        while (true)
        {
            ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
            if (bytes_read < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 没有数据可读，继续等待
                    continue;
                } else {
                    std::cerr << "Failed to read from socket: " << strerror(errno) << std::endl;
                    break;
                }
            } else if (bytes_read == 0) {
                // 连接关闭
                break;
            }
            
            // todo 一个协议超过4096字节
            if (auto err = parser.ExecuteParse(buffer, bytes_read); err.has_value())
            {
                // 解析错误，处理错误
                std::cerr << "Parse error: " << err.value().What() << std::endl;
                break;
            }

            std::cout << "http req: " << std::endl;
            std::cout << buffer << std::endl;

            // 发送响应
            ssize_t bytes_written = write(fd, response_str, strlen(response_str));
            if (bytes_written < 0) {
                std::cerr << "Failed to write response: " << strerror(errno) << std::endl;
                break; // 发送失败，退出循环
            }
        }

        // 关闭客户端连接
        close(fd);
        std::cout << "Client disconnected." << std::endl;
    }

private:
    int m_listen_fd{-1};
    std::string m_listen_addr;
    int m_listen_port{-1};

    volatile bool m_is_running{true};
};

int main()
{
    g_bbt_coroutine_config->m_cfg_stack_size = 32 * 1024; // 设置协程栈大小
    g_bbt_coroutine_config->m_cfg_max_coroutine = 3000;

    g_scheduler->Start(); // 启动调度器

    HttpServer server("0.0.0.0", 8080);
    server.Run(); // 启动 HTTP 服务器

    g_scheduler->Stop();
}