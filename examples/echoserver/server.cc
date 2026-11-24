#include <cerrno>
#include <cstdio>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

void Echo(int fd)
{
    bbtco_defer
    {
        ::close(fd);
    };
    char buffer[512];
    for (;;) {
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return;
        for (ssize_t sent = 0; sent < count;) {
            const auto n = ::send(fd, buffer + sent, count - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return;
            sent += n;
        }
    }
}

int bbtco_main()
{
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        std::perror("socket");
        return 1;
    }
    bbtco_defer
    {
        ::close(listener);
    };

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(10010);
    const int reuse = 1;
    if (::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0
        || ::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
        || ::listen(listener, SOMAXCONN) != 0) {
        std::perror("listen on 127.0.0.1:10010");
        return 1;
    }

    for (;;) {
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            std::perror("accept");
            return 1;
        }
        bool started = false;
        bbtco_noexcept(&started)[client]()
        {
            Echo(client);
        };
        if (!started) {
            ::close(client);
            std::fputs("could not start echo coroutine\n", stderr);
        }
    }
}

BBTCO_MAIN()
