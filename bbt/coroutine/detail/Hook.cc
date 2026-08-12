#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine::detail
{

namespace
{

/**
 * @brief 协程层挂起失败的 errno 恢复守卫（#190）
 *
 * 系统调用返回可重试错误（EAGAIN 等）时记录其 errno，随后协程层操作
 * （事件注册、epoll、上下文切换）可能覆盖 errno。若协程层失败或抛异常，
 * 调用方看到的不应是协程内部 errno，而是原始系统调用的 errno。
 * 异常路径恢复 errno 后重新抛出（对调用方透明）。
 */
class ErrnoGuard
{
public:
    explicit ErrnoGuard(int sys_errno): m_sys_errno(sys_errno) {}
    ~ErrnoGuard() { errno = m_sys_errno; }
private:
    int m_sys_errno;
};

/**
 * @brief 常规文件读写的偏移回退守卫（#190）
 *
 * 仅当 fd 是常规文件且可 seek 时记录初始偏移；析构时若尚未解除
 * （异常路径）则回退偏移，保证 hook 内部异常不会造成偏移前进。
 */
class FileOffsetGuard
{
public:
    explicit FileOffsetGuard(int fd)
    {
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
            return;

        auto offset = lseek(fd, 0, SEEK_CUR);
        if (offset >= 0) {
            m_fd = fd;
            m_offset = offset;
            m_active = true;
        }
    }

    ~FileOffsetGuard()
    {
        if (m_active)
            lseek(m_fd, m_offset, SEEK_SET);
    }

    /* 正常完成时解除，保留系统调用自身推进的偏移 */
    void Dismiss() noexcept { m_active = false; }

private:
    int     m_fd{-1};
    off_t   m_offset{0};
    bool    m_active{false};
};

} // namespace

int Hook_Socket(int domain, int type, int protocol)
{
    int fd = -1;

    fd = g_bbt_sys_hook_socket_func(domain, type, protocol);
    if (fd < 0)
        return -1;

    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != 0) {
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

        int sys_errno = errno;
        ErrnoGuard guard{sys_errno};

        try {
            if (g_bbt_tls_coroutine_co->YieldUntilFdWriteable(socket) != 0)
                return -1;
        } catch (...) {
            throw;
        }
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
    FileOffsetGuard offset_guard{fd};

    while ((read_len = g_bbt_sys_hook_read_func(fd, buf, nbytes)) < 0) {
        /* 如果read没有立即成功，判断失败原因是否为正在执行读操作 */
        if (errno != EAGAIN && errno != EINPROGRESS && errno != EINTR && errno != EWOULDBLOCK)
            return -1;

        int sys_errno = errno;
        ErrnoGuard guard{sys_errno};

        try {
            /* 对当前协程注册fd可读事件，挂起当前协程直到fd可读 */
            if (g_bbt_tls_coroutine_co->YieldUntilFdReadable(fd) != 0)
                return -1;
        } catch (...) {
            throw;
        }
    }

    offset_guard.Dismiss();
    return read_len;
}

ssize_t Hook_Write(int fd, const void *buf, size_t n)
{
    ssize_t write_len = -1;
    FileOffsetGuard offset_guard{fd};

    while ((write_len = g_bbt_sys_hook_write_func(fd, buf, n)) < 0) {
        /* 如果write没有立即成功，判断失败原因是否为正在执行写操作 */
        if (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK)
            return -1;

        int sys_errno = errno;
        ErrnoGuard guard{sys_errno};

        try {
            /* 对当前协程注册fd可写事件，挂起当前协程直到fd可写 */
            if (g_bbt_tls_coroutine_co->YieldUntilFdWriteable(fd) != 0)
                return -1;
        } catch (...) {
            throw;
        }
    }

    offset_guard.Dismiss();
    return write_len;
}

int Hook_Accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    int new_cli_fd = -1;

    while ((new_cli_fd = g_bbt_sys_hook_accept_func(fd, addr, len)) < 0) {
        /* 如果accept没有立即成功，判断失败原因是否为设置非阻塞 */
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return -1;

        int sys_errno = errno;
        ErrnoGuard guard{sys_errno};

        try {
            /* 对当前协程注册fd可读事件，挂起当前协程直到fd可读 */
            if (g_bbt_tls_coroutine_co->YieldUntilFdReadable(fd) != 0)
                return -1;
        } catch (...) {
            throw;
        }
    }

    if (fcntl(new_cli_fd, F_SETFL, fcntl(new_cli_fd, F_GETFL, 0) | O_NONBLOCK) != 0) {
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

        int sys_errno = errno;
        ErrnoGuard guard{sys_errno};

        try {
            /* 对当前协程注册fd可写事件，挂起当前协程直到fd可写 */
            if (g_bbt_tls_coroutine_co->YieldUntilFdWriteable(fd) != 0)
                return -1;
        } catch (...) {
            throw;
        }
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

        int sys_errno = errno;
        ErrnoGuard guard{sys_errno};

        try {
            /* 对当前协程注册fd可读事件，挂起当前协程直到fd可读 */
            if (g_bbt_tls_coroutine_co->YieldUntilFdReadable(fd) != 0)
                return -1;
        } catch (...) {
            throw;
        }
    }

    return recv_len;
}

ssize_t Hook_SendTo(int fd, const void *buf, size_t len, int flags, const struct sockaddr* dest_addr, socklen_t addrlen)
{
    ssize_t send_len = -1;
    while ((send_len = g_bbt_sys_hook_sendto_func(fd, buf, len, flags, dest_addr, addrlen)) < 0) {
        if (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK)
            return -1;

        int sys_errno = errno;
        ErrnoGuard guard{sys_errno};

        try {
            /* 对当前协程注册fd可写事件，挂起当前协程直到fd可写 */
            if (g_bbt_tls_coroutine_co->YieldUntilFdWriteable(fd) != 0)
                return -1;
        } catch (...) {
            throw;
        }
    }

    return send_len;
}

ssize_t Hook_RecvFrom(int fd, void *buf, size_t len, int flags, struct sockaddr* src_addr, socklen_t* addrlen)
{
    ssize_t recv_len = -1;
    while ((recv_len = g_bbt_sys_hook_recvfrom_func(fd, buf, len, flags, src_addr, addrlen)) < 0) {
        if (errno != EAGAIN && errno != EINPROGRESS && errno != EINTR && errno != EWOULDBLOCK)
            return -1;

        int sys_errno = errno;
        ErrnoGuard guard{sys_errno};

        try {
            /* 对当前协程注册fd可读事件，挂起当前协程直到fd可读 */
            if (g_bbt_tls_coroutine_co->YieldUntilFdReadable(fd) != 0)
                return -1;
        } catch (...) {
            throw;
        }
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

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr* dest_addr, socklen_t addrlen)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_sendto_func(fd, buf, len, flags, dest_addr, addrlen);

    return bbt::coroutine::detail::Hook_SendTo(fd, buf, len, flags, dest_addr, addrlen);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr* src_addr, socklen_t* addrlen)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_recvfrom_func(fd, buf, len, flags, src_addr, addrlen);

    return bbt::coroutine::detail::Hook_RecvFrom(fd, buf, len, flags, src_addr, addrlen);
}
