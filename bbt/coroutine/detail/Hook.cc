#include <stdio.h>
#include <event2/util.h>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/WaitProtocol.hpp>

namespace bbt::coroutine::detail
{

using EventOpt = bbt::pollevent::EventOpt;

namespace
{

bool ShouldRetry(int err, std::initializer_list<int> retryable_errors)
{
    for (int candidate : retryable_errors) {
        if (err == candidate)
            return true;
    }

    return false;
}

WaitResult WaitFdReady(int fd, short event)
{
    return WaitProtocolBridge::WaitFd(*g_bbt_tls_coroutine_co, fd, event | EventOpt::FINALIZE);
}

template <typename Ret, typename Fn>
Ret RetryFdIo(int fd, short event, std::initializer_list<int> retryable_errors, Fn&& fn)
{
    Ret io_ret = -1;
    while ((io_ret = fn()) < 0) {
        if (!ShouldRetry(errno, retryable_errors))
            return -1;

        if (WaitFdReady(fd, event) == WaitResult::WAIT_ERROR)
            return -1;
    }

    return io_ret;
}

}

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
        if (!ShouldRetry(errno, {EINTR, EINPROGRESS, EALREADY}))
            return -1;

        if (WaitFdReady(socket, EventOpt::WRITEABLE) == WaitResult::WAIT_ERROR)
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

    return WaitProtocolBridge::WaitTimeout(*g_bbt_tls_coroutine_co, ms) == WaitResult::WAIT_ERROR ? -1 : 0;
}

ssize_t Hook_Read(int fd, void *buf, size_t nbytes)
{
    return RetryFdIo<ssize_t>(fd,
                              EventOpt::READABLE,
                              {EAGAIN, EINPROGRESS, EINTR, EWOULDBLOCK},
                              [&]() { return g_bbt_sys_hook_read_func(fd, buf, nbytes); });
}

ssize_t Hook_Write(int fd, const void *buf, size_t n)
{
    return RetryFdIo<ssize_t>(fd,
                              EventOpt::WRITEABLE,
                              {EAGAIN, EINTR, EWOULDBLOCK},
                              [&]() { return g_bbt_sys_hook_write_func(fd, buf, n); });
}

int Hook_Accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    int new_cli_fd = RetryFdIo<int>(fd,
                                    EventOpt::READABLE,
                                    {EAGAIN, EWOULDBLOCK, EINTR},
                                    [&]() { return g_bbt_sys_hook_accept_func(fd, addr, len); });
    if (new_cli_fd < 0)
        return -1;

    if (evutil_make_socket_nonblocking(new_cli_fd) != 0) {
        ::close(new_cli_fd);
        new_cli_fd = -1;
    }

    return new_cli_fd;
}

ssize_t Hook_Send(int fd, const void *buf, size_t n, int flags)
{
    return RetryFdIo<ssize_t>(fd,
                              EventOpt::WRITEABLE,
                              {EAGAIN, EINTR, EWOULDBLOCK},
                              [&]() { return g_bbt_sys_hook_send_func(fd, buf, n, flags); });
}

ssize_t Hook_Recv(int fd, void *buf, size_t n, int flags)
{
    return RetryFdIo<ssize_t>(fd,
                              EventOpt::READABLE,
                              {EAGAIN, EINPROGRESS, EINTR, EWOULDBLOCK},
                              [&]() { return g_bbt_sys_hook_recv_func(fd, buf, n, flags); });
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
