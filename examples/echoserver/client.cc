#include <cerrno>
#include <cstdio>
#include <string_view>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

int bbtco_main()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return 1;
    }
    bbtco_defer
    {
        ::close(fd);
    };

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(10010);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        std::perror("connect to 127.0.0.1:10010");
        return 1;
    }

    constexpr std::string_view message = "hello";
    for (std::size_t sent = 0; sent < message.size();) {
        const auto n = ::send(fd, message.data() + sent, message.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 1;
        sent += n;
    }

    char reply[message.size()];
    for (std::size_t received = 0; received < sizeof(reply);) {
        const auto n = ::recv(fd, reply + received, sizeof(reply) - received, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 1;
        received += n;
    }
    if (std::string_view(reply, sizeof(reply)) != message)
        return 1;
    std::printf("echo: %.*s\n", static_cast<int>(sizeof(reply)), reply);
    return 0;
}

BBTCO_MAIN()
