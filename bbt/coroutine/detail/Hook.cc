#include <stdio.h>
#include <event2/util.h>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine::detail
{

int Hook_Socket(int domain, int type, int protocol)
{
    int fd = -1;

    fd = g_bbt_sys_hook_socket_func(domain, type, protocol);
    if (fd < 0)
        return -1;

    if (evutil_make_socket_nonblocking(fd) != 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

int Hook_Connect(int socket, const struct sockaddr *address, socklen_t address_len)
{

    while (g_bbt_sys_hook_connect_func(socket, address, address_len) != 0) {
        // 是否因为非阻塞导致没法立即完成
        if (errno != EINTR && errno != EINPROGRESS && errno != EALREADY)
            return -1;

        if (g_bbt_tls_coroutine_co->YieldUntilFdWriteable(socket) != 0)
            return -1;
    }

    return 0;
}

int Hook_Close(int fd)
{
    return g_bbt_sys_hook_close_func(fd);
}

int Hook_Sleep(int ms)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "must be in coroutine context");
    if (ms <= 0)
        return -1;

    return g_bbt_tls_coroutine_co->YieldUntilTimeout(ms);
}

ssize_t Hook_Read(int fd, void *buf, size_t nbytes)
{
    ssize_t read_len = -1;
    while ((read_len = g_bbt_sys_hook_read_func(fd, buf, nbytes)) < 0) {
        /* 如果read没有立即成功，判断失败原因是否为正在执行读操作 */
        if (errno != EAGAIN && errno != EINPROGRESS && errno != EINTR && errno != EWOULDBLOCK)
            return -1;

        /* 对当前协程注册fd可读事件，挂起当前协程直到fd可读 */
        if (g_bbt_tls_coroutine_co->YieldUntilFdReadable(fd) != 0)
            return -1;

    }

    return read_len;
}

ssize_t Hook_Write(int fd, const void *buf, size_t n)
{
    ssize_t write_len = -1;
    while ((write_len = g_bbt_sys_hook_write_func(fd, buf, n)) < 0) {
        /* 如果write没有立即成功，判断失败原因是否为正在执行写操作 */
        if (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK)
            return -1;

        /* 对当前协程注册fd可写事件，挂起当前协程直到fd可写 */
        if (g_bbt_tls_coroutine_co->YieldUntilFdWriteable(fd) != 0)
            return -1;
    }

    return write_len;
}

int Hook_Accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    int new_cli_fd = -1;

    while ((new_cli_fd = g_bbt_sys_hook_accept_func(fd, addr, len)) < 0) {
        /* 如果accept没有立即成功，判断失败原因是否为设置非阻塞 */
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return -1;

        /* 对当前协程注册fd可读事件，挂起当前协程直到fd可读 */
        if (g_bbt_tls_coroutine_co->YieldUntilFdReadable(fd) != 0)
            return -1;
        
    }

    if (evutil_make_socket_nonblocking(new_cli_fd) != 0) {
        ::close(new_cli_fd);
        new_cli_fd = -1;
    }

    return new_cli_fd;
}

ssize_t Hook_Send(int fd, const void *buf, size_t n, int flags)
{
    ssize_t send_len = -1;
    while ((send_len = g_bbt_sys_hook_send_func(fd, buf, n, flags)) < 0) {
        /* 如果write没有立即成功，判断失败原因是否为正在执行写操作 */
        if (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK)
            return -1;

        /* 对当前协程注册fd可写事件，挂起当前协程直到fd可写 */
        if (g_bbt_tls_coroutine_co->YieldUntilFdWriteable(fd) != 0)
            return -1;
    }

    return send_len;
}

ssize_t Hook_Recv(int fd, void *buf, size_t n, int flags)
{
    ssize_t recv_len = -1;
    while ((recv_len = g_bbt_sys_hook_recv_func(fd, buf, n, flags)) < 0) {
        /* 如果read没有立即成功，判断失败原因是否为正在执行读操作 */
        if (errno != EAGAIN && errno != EINPROGRESS && errno != EINTR && errno != EWOULDBLOCK)
            return -1;

        /* 对当前协程注册fd可读事件，挂起当前协程直到fd可读 */
        if (g_bbt_tls_coroutine_co->YieldUntilFdReadable(fd) != 0)
            return -1;
    }

    return recv_len;
}

}

int socket(int domain, int type, int protocol)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_socket_func(domain, type, protocol);

    return bbt::coroutine::detail::Hook_Socket(domain, type, protocol);
}

int connect(int socket, const struct sockaddr *address, socklen_t address_len)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_connect_func(socket, address, address_len);

    return bbt::coroutine::detail::Hook_Connect(socket, address, address_len);
}

int close(int fd)
{
    return bbt::coroutine::detail::Hook_Close(fd);
}

unsigned int sleep(unsigned int sec)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_sleep_func(sec);

    return bbt::coroutine::detail::Hook_Sleep(sec * 1000);
}

ssize_t read(int fd, void *buf, size_t nbytes)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_read_func(fd, buf, nbytes);

    return bbt::coroutine::detail::Hook_Read(fd, buf, nbytes);
}

ssize_t write(int fd, const void *buf, size_t n)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_write_func(fd, buf, n);

    return bbt::coroutine::detail::Hook_Write(fd, buf, n);
}

int accept(int fd, __SOCKADDR_ARG addr, socklen_t *__restrict addr_len)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_accept_func(fd, addr, addr_len);

    return bbt::coroutine::detail::Hook_Accept(fd, addr, addr_len);
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{

    if (!g_bbt_tls_helper->EnableUseCo())
    {
        return g_bbt_sys_hook_send_func(fd, buf, len, flags);
    }

    return bbt::coroutine::detail::Hook_Send(fd, buf, len, flags);
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{

    if (!g_bbt_tls_helper->EnableUseCo())
    {
        return g_bbt_sys_hook_recv_func(fd, buf, len, flags);
    }

    return bbt::coroutine::detail::Hook_Recv(fd, buf, len, flags);
}