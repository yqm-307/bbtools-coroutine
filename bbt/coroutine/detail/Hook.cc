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
 * @brief accept4 降级路径：对 accept 出的新 fd 补 SOCK_CLOEXEC/SOCK_NONBLOCK（#228）
 *
 * WHY: dlsym 取不到 accept4 时，非协程直通与协程两条路径都要手工补 flags，逻辑共用一份。
 */
int ApplyAccept4Flags(int fd, int flags)
{
    if (flags & SOCK_CLOEXEC) {
        int fl = ::fcntl(fd, F_GETFD, 0);
        if (fl < 0 || ::fcntl(fd, F_SETFD, fl | FD_CLOEXEC) != 0)
            return -1;
    }
    if ((flags & SOCK_NONBLOCK) &&
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != 0)
        return -1;
    return 0;
}

/**
 * @brief 重置非阻塞 connect 的 socket 状态（#191）
 *
 * 非阻塞 connect 失败后，socket 的错误结果保存在 SO_ERROR 中；
 * 读取 SO_ERROR 后内核释放连接状态，socket 回到未连接、可重新 connect。
 * connect 挂起路径被异常打断或协程层失败时调用，保证 socket 可重试。
 */
void ResetConnectState(int fd) noexcept
{
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
}

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
        // EISCONN：连接已建立（非阻塞 connect 完成后重入），视为成功
        if (errno == EISCONN)
            return 0;

        // 是否因为非阻塞导致没法立即完成
        if (errno != EINTR && errno != EINPROGRESS && errno != EALREADY)
            return -1;

        int sys_errno = errno;
        ErrnoGuard guard{sys_errno};

        try {
            if (g_bbt_tls_coroutine_co->YieldUntilFdWriteable(socket) != 0) {
                // 协程层失败：重置 socket 状态，使其回到未连接、可重试
                ResetConnectState(socket);
                return -1;
            }
        } catch (...) {
            // 异常路径：同样重置，保证调用方（如协程取消后）可重用 socket
            ResetConnectState(socket);
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

ssize_t Hook_RecvMsg(int fd, struct msghdr *msg, int flags)
{
    /* MSG_DONTWAIT：调用方明确要求不等待，协程内也直通原函数（#228） */
    if (flags & MSG_DONTWAIT)
        return g_bbt_sys_hook_recvmsg_func(fd, msg, flags);

    ssize_t recv_len = -1;
    while ((recv_len = g_bbt_sys_hook_recvmsg_func(fd, msg, flags)) < 0) {
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

ssize_t Hook_SendMsg(int fd, const struct msghdr *msg, int flags)
{
    /* 同 Hook_RecvMsg：MSG_DONTWAIT 直通 */
    if (flags & MSG_DONTWAIT)
        return g_bbt_sys_hook_sendmsg_func(fd, msg, flags);

    ssize_t send_len = -1;
    while ((send_len = g_bbt_sys_hook_sendmsg_func(fd, msg, flags)) < 0) {
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

ssize_t Hook_Readv(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t read_len = -1;
    /* 与 Hook_Read 相同：常规文件偏移回退守卫；iovec 整体注册事件，不拆段提交（#228） */
    FileOffsetGuard offset_guard{fd};

    while ((read_len = g_bbt_sys_hook_readv_func(fd, iov, iovcnt)) < 0) {
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

ssize_t Hook_Writev(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t write_len = -1;
    FileOffsetGuard offset_guard{fd};

    while ((write_len = g_bbt_sys_hook_writev_func(fd, iov, iovcnt)) < 0) {
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

/**
 * @brief 协程内 accept4（#228）
 *
 * dlsym 取不到 accept4 时降级为 accept 后再补 SOCK_CLOEXEC/SOCK_NONBLOCK。
 * 成功路径与 Hook_Accept 一致：新 fd 强制 O_NONBLOCK，供后续协程 IO 走挂起路径。
 */
int Hook_Accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags)
{
    int new_cli_fd = -1;

    if (g_bbt_sys_hook_accept4_func) {
        while ((new_cli_fd = g_bbt_sys_hook_accept4_func(fd, addr, len, flags)) < 0) {
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
    } else {
        /* 降级：accept + 手工补 flags（平台无 accept4 或 dlsym 取不到） */
        new_cli_fd = Hook_Accept(fd, addr, len);
        if (new_cli_fd < 0)
            return -1;
        if (ApplyAccept4Flags(new_cli_fd, flags) != 0) {
            ::close(new_cli_fd);
            return -1;
        }
        return new_cli_fd;
    }

    if (fcntl(new_cli_fd, F_SETFL, fcntl(new_cli_fd, F_GETFL, 0) | O_NONBLOCK) != 0) {
        ::close(new_cli_fd);
        new_cli_fd = -1;
    }

    return new_cli_fd;
}

/**
 * @brief 协程内 usleep（#229）
 *
 * 时长向上取整到毫秒（YieldUntilTimeout 单位是 ms）；0 时长立即成功返回，
 * 不复用 Hook_Sleep 的 ms<=0 → -1 语义（usleep(0) 在 POSIX 是合法空操作）。
 */
int Hook_USleep(unsigned int usec)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "must be in coroutine context");
    if (usec == 0)
        return 0;

    return g_bbt_tls_coroutine_co->YieldUntilTimeout((usec + 999) / 1000);
}

/**
 * @brief 协程内 nanosleep（#229）
 *
 * req==NULL 直通原函数（由 libc 给出 -1/EINVAL，hook 不模仿 errno）。
 * 成功睡完返回 0；不写 rem——POSIX 规定成功路径 rem 内容未定义，
 * 且协程定时器不会以 EINTR 提前结束，没有剩余时间可报告。
 */
int Hook_Nanosleep(const struct timespec *req, struct timespec *rem)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "must be in coroutine context");
    if (req == nullptr)
        return g_bbt_sys_hook_nanosleep_func(req, rem);

    long long ms = (long long)req->tv_sec * 1000 + (req->tv_nsec + 999999LL) / 1000000LL;
    if (ms <= 0)
        return 0;

    return g_bbt_tls_coroutine_co->YieldUntilTimeout((int)ms);
}

/**
 * @brief 协程内 clock_nanosleep（#229）
 *
 * 只有 CLOCK_MONOTONIC / CLOCK_REALTIME 能映射到 YieldUntilTimeout 的墙钟超时；
 * CPU 时间钟等其它 clock_id 即使在协程内也直通原函数，不能用墙钟冒充其语义。
 * TIMER_ABSTIME 先用 clock_gettime 折算成相对剩余时间，已到期立即返回 0。
 * 注意 clock_nanosleep 以返回值报告错误、不设置 errno，直通路径原样透传。
 */
int Hook_ClockNanosleep(clockid_t clock_id, int flags, const struct timespec *req, struct timespec *rem)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "must be in coroutine context");
    if (req == nullptr)
        return g_bbt_sys_hook_clock_nanosleep_func(clock_id, flags, req, rem);

    if (clock_id != CLOCK_MONOTONIC && clock_id != CLOCK_REALTIME)
        return g_bbt_sys_hook_clock_nanosleep_func(clock_id, flags, req, rem);

    long long ms;
    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        if (clock_gettime(clock_id, &now) != 0)
            return errno;
        long long remain_ns = ((long long)req->tv_sec - now.tv_sec) * 1000000000LL
                            + ((long long)req->tv_nsec - now.tv_nsec);
        if (remain_ns <= 0)
            return 0;
        ms = (remain_ns + 999999LL) / 1000000LL;
    } else {
        ms = (long long)req->tv_sec * 1000 + (req->tv_nsec + 999999LL) / 1000000LL;
        if (ms <= 0)
            return 0;
    }

    // POSIX: clock_nanosleep 成功返回 0，失败返回正 errno，不用 -1。
    if (g_bbt_tls_coroutine_co->YieldUntilTimeout((int)ms) != 0)
        return EINVAL;
    return 0;
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

ssize_t recvmsg(int fd, struct msghdr *message, int flags)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_recvmsg_func(fd, message, flags);

    return bbt::coroutine::detail::Hook_RecvMsg(fd, message, flags);
}

ssize_t sendmsg(int fd, const struct msghdr *message, int flags)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_sendmsg_func(fd, message, flags);

    return bbt::coroutine::detail::Hook_SendMsg(fd, message, flags);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_readv_func(fd, iov, iovcnt);

    return bbt::coroutine::detail::Hook_Readv(fd, iov, iovcnt);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_writev_func(fd, iov, iovcnt);

    return bbt::coroutine::detail::Hook_Writev(fd, iov, iovcnt);
}

int accept4(int fd, __SOCKADDR_ARG addr, socklen_t *__restrict addr_len, int flags)
{
    if (!g_bbt_tls_helper->EnableUseCo()) {
        if (g_bbt_sys_hook_accept4_func)
            return g_bbt_sys_hook_accept4_func(fd, addr, addr_len, flags);
        /* 降级直通：accept + 手工补 flags，不引入协程挂起逻辑 */
        int new_fd = g_bbt_sys_hook_accept_func(fd, addr, addr_len);
        if (new_fd >= 0 && bbt::coroutine::detail::ApplyAccept4Flags(new_fd, flags) != 0) {
            ::close(new_fd);
            return -1;
        }
        return new_fd;
    }

    return bbt::coroutine::detail::Hook_Accept4(fd, addr, addr_len, flags);
}

int usleep(useconds_t usec)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_usleep_func(usec);

    return bbt::coroutine::detail::Hook_USleep(usec);
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_nanosleep_func(req, rem);

    return bbt::coroutine::detail::Hook_Nanosleep(req, rem);
}

int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec *req, struct timespec *rem)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_clock_nanosleep_func(clock_id, flags, req, rem);

    return bbt::coroutine::detail::Hook_ClockNanosleep(clock_id, flags, req, rem);
}
