#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <string>
#include <vector>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/DnsResolver.hpp>
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
 * @brief epoll fd 的 RAII 守卫（#230）
 *
 * WHY: PollCore 的挂起路径有多个提前返回点，手写 ::close 容易漏；
 * epfd 是本函数自建的普通 fd，直接 ::close（同 TU 的 hook close→dlsym 直通，无递归）。
 */
class EpGuard
{
public:
    explicit EpGuard(int fd): m_fd(fd) {}
    ~EpGuard() { if (m_fd >= 0) ::close(m_fd); }
    EpGuard(const EpGuard&) = delete;
    EpGuard& operator=(const EpGuard&) = delete;
private:
    int m_fd;
};

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
 * 协程 IO 期间临时 O_NONBLOCK，返回时还原调用方 flags（#260）。
 * 设不了非阻塞则失败返回，禁止在 blocking fd 上卡住 worker。
 * ponytail: 按次还原；多协程共享同一 blocking fd 可能竞态。
 */
class CoIoNonblockGuard
{
public:
    explicit CoIoNonblockGuard(int fd): m_fd(fd)
    {
        m_old = ::fcntl(fd, F_GETFL, 0);
        if (m_old < 0) {
            m_ok = false;
            return;
        }
        if (m_old & O_NONBLOCK) {
            m_ok = true;
            return;
        }
        if (::fcntl(fd, F_SETFL, m_old | O_NONBLOCK) != 0) {
            m_ok = false;
            return;
        }
        m_restore = true;
        m_ok = true;
    }
    ~CoIoNonblockGuard()
    {
        if (m_restore)
            ::fcntl(m_fd, F_SETFL, m_old);
    }
    CoIoNonblockGuard(const CoIoNonblockGuard&) = delete;
    CoIoNonblockGuard& operator=(const CoIoNonblockGuard&) = delete;
    bool ok() const { return m_ok; }
private:
    int  m_fd{-1};
    int  m_old{0};
    bool m_ok{false};
    bool m_restore{false};
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
    CoIoNonblockGuard io{socket};
    if (!io.ok())
        return -1;

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
    CoIoNonblockGuard io{fd};
    if (!io.ok())
        return -1;

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
    CoIoNonblockGuard io{fd};
    if (!io.ok())
        return -1;

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
    CoIoNonblockGuard io{fd};
    if (!io.ok())
        return -1;

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
    CoIoNonblockGuard io{fd};
    if (!io.ok())
        return -1;

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
    CoIoNonblockGuard io{fd};
    if (!io.ok())
        return -1;

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
    CoIoNonblockGuard io{fd};
    if (!io.ok())
        return -1;

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
    CoIoNonblockGuard io{fd};
    if (!io.ok())
        return -1;

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

    CoIoNonblockGuard io{fd};
    if (!io.ok())
        return -1;

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

    CoIoNonblockGuard io{fd};
    if (!io.ok())
        return -1;

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
    CoIoNonblockGuard io{fd};
    if (!io.ok())
        return -1;

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
    CoIoNonblockGuard io{fd};
    if (!io.ok())
        return -1;

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
    CoIoNonblockGuard io{fd};
    if (!io.ok())
        return -1;

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

/**
 * @brief 协程内 poll 核心算法（#230）
 *
 * 事件流：native poll(0) 快照 → 无就绪且需要等待时，建一个不被 hook 的 epoll fd
 * 聚合所有有 POLLIN/POLLOUT/POLLPRI 兴趣的 fd → 协程挂起等 epoll fd 可读 →
 * 醒来后 native poll(0) 填 revents。epoll 是 level-triggered：快照与注册之间到达
 * 的事件不会丢，挂起时 epfd 仍可读会立即唤醒。
 *
 * WHY 内部只走 g_bbt_sys_hook_poll_func（dlsym 原函数）：调度器经 libevent/epoll 驱动、
 * 不走 poll，但 hook 内若调 ::poll 会重入本实现造成无限递归。epoll_create1/epoll_ctl
 * 未被 hook，对 epfd 的 YieldUntilFdReadable 走既有 CoPollEvent 路径。
 *
 * epoll_ctl ADD 失败的 fd（普通文件等 epoll 不支持的类型）不进 epoll；
 * 若无任何 fd 能进 epoll，不挂起，直接返回快照结果。
 */
static int PollCore(struct pollfd* fds, nfds_t nfds, int timeout_ms)
{
    /* 快照：poll(2) 按 POSIX 重写全部 revents；出错（EBADF 等）时 errno 即原生 poll 的 */
    int ret = g_bbt_sys_hook_poll_func(fds, nfds, 0);
    if (ret < 0)
        return ret;
    int ready = 0;
    for (nfds_t i = 0; i < nfds; ++i)
        if (fds[i].revents != 0)
            ++ready;
    if (ready > 0 || timeout_ms == 0)
        return ready;

    /* 纯定时：无任何 fd 可监听，睡满 timeout 后返回 0 */
    if (nfds == 0) {
        if (timeout_ms > 0 && g_bbt_tls_coroutine_co->YieldUntilTimeout(timeout_ms) != 0)
            return -1;
        return 0;
    }

    auto Interest = [](short events) {
        return events & (POLLIN | POLLOUT | POLLPRI);
    };

    int epfd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
        return -1;
    EpGuard ep_guard{epfd};  // RAII 关 epoll

    int ep_count = 0;
    for (nfds_t i = 0; i < nfds; ++i) {
        if (!Interest(fds[i].events))
            continue;
        struct epoll_event ev;
        std::memset(&ev, 0, sizeof(ev));
        if (fds[i].events & POLLIN)
            ev.events |= EPOLLIN;
        if (fds[i].events & POLLOUT)
            ev.events |= EPOLLOUT;
        if (fds[i].events & POLLPRI)
            ev.events |= EPOLLPRI;
        ev.data.u32 = static_cast<uint32_t>(i);
        if (::epoll_ctl(epfd, EPOLL_CTL_ADD, fds[i].fd, &ev) == 0)
            ++ep_count;
    }

    if (ep_count > 0) {
        /* timeout<0 无限等；timeout>0 带协程定时器。醒来（或定时器到期）后统一走快照收尾 */
        int yret = timeout_ms < 0
            ? g_bbt_tls_coroutine_co->YieldUntilFdReadable(epfd)
            : g_bbt_tls_coroutine_co->YieldUntilFdReadable(epfd, timeout_ms);
        if (yret != 0)
            return -1;
    }

    ret = g_bbt_sys_hook_poll_func(fds, nfds, 0);
    if (ret < 0)
        return ret;
    ready = 0;
    for (nfds_t i = 0; i < nfds; ++i)
        if (fds[i].revents != 0)
            ++ready;
    /* ep_count==0：全是 epoll 不支持的 fd，按快照直接返回，不挂起 */
    return ready;
}

/**
 * @brief 协程内 poll（#230）
 */
int Hook_Poll(struct pollfd* fds, nfds_t nfds, int timeout_ms)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "must be in coroutine context");
    return PollCore(fds, nfds, timeout_ms);
}

/**
 * @brief fd_set → pollfd[] 转换后走 PollCore，再写回 fd_set（#230）
 *
 * select 与 pselect 协程路径共用；timeout_ms 传 -1 表示无限等待。
 * 写回映射取 glibc __select 兼容表的口径（POLLHUP/ERR/NVAL 同时点亮读写集）。
 */
static int SelectCore(int nfds, fd_set* read_fds, fd_set* write_fds, fd_set* except_fds, int timeout_ms)
{
    if (nfds < 0)
        nfds = 0;
    if (nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }

    std::vector<pollfd> pfds;
    for (int fd = 0; fd < nfds; ++fd) {
        short events = 0;
        if (read_fds && FD_ISSET(fd, read_fds))
            events |= POLLIN;
        if (write_fds && FD_ISSET(fd, write_fds))
            events |= POLLOUT;
        if (except_fds && FD_ISSET(fd, except_fds))
            events |= POLLPRI;
        if (events == 0)
            continue;
        pfds.push_back(pollfd{fd, events, 0});
    }

    int ret = PollCore(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout_ms);
    if (ret < 0)
        return ret;

    for (fd_set* s : {read_fds, write_fds, except_fds})
        if (s)
            FD_ZERO(s);
    for (const pollfd& p : pfds) {
        if (read_fds && (p.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
            FD_SET(p.fd, read_fds);
        if (write_fds && (p.revents & (POLLOUT | POLLHUP | POLLERR | POLLNVAL)))
            FD_SET(p.fd, write_fds);
        if (except_fds && (p.revents & POLLPRI))
            FD_SET(p.fd, except_fds);
    }

    int counted = 0;
    for (fd_set* s : {read_fds, write_fds, except_fds})
        if (s)
            for (int fd = 0; fd < nfds; ++fd)
                if (FD_ISSET(fd, s))
                    ++counted;
    return counted;
}

/**
 * @brief 协程内 select（#230）
 *
 * timeval → 毫秒向上取整；NULL 表示无限等待。不回写剩余时间——
 * 协程定时器到期即结束，没有可报告的 rem。
 */
int Hook_Select(int nfds, fd_set* read_fds, fd_set* write_fds, fd_set* except_fds, struct timeval* timeout)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "must be in coroutine context");
    int timeout_ms = -1;
    if (timeout != nullptr)
        timeout_ms = static_cast<int>(timeout->tv_sec) * 1000 +
                     static_cast<int>((timeout->tv_usec + 999) / 1000);
    return SelectCore(nfds, read_fds, write_fds, except_fds, timeout_ms);
}

/**
 * @brief 协程内 pselect（#230）
 *
 * ponytail: 协程路径忽略 sigmask——worker 线程 pthread_sigmask 会伤同核其它协程；
 * 要按 fd 精确屏蔽再加。timeout NULL 表示无限等待。
 */
int Hook_PSelect(int nfds, fd_set* read_fds, fd_set* write_fds, fd_set* except_fds, const struct timespec* timeout, const sigset_t* sigmask)
{
    (void)sigmask;
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "must be in coroutine context");
    int timeout_ms = -1;
    if (timeout != nullptr)
        timeout_ms = static_cast<int>(timeout->tv_sec) * 1000 +
                     static_cast<int>((timeout->tv_nsec + 999999) / 1000000);
    return SelectCore(nfds, read_fds, write_fds, except_fds, timeout_ms);
}

/**
 * @brief gethostbyname/gethostbyaddr 协程路径的 thread_local hostent 存储（#231）
 *
 * WHY thread_local：libc 的 hostent 静态缓冲按线程共享，worker 线程里直接返回
 * libc 指针会与调用线程的后续调用互相踩内存；hostent 的字符串/地址数组必须与
 * hostent 结构同线程、同生命周期，所以全部收进这一份 TLS 存储，返回其内部指针。
 * 存储放在 TLS 而非协程栈：gethostbyname 契约是"结果有效直到下次调用"，
 * 协程栈在 hook 返回后即析构。
 */
struct TlsHostentStorage
{
    struct hostent      ent;
    std::string         name;
    std::vector<std::string> aliases;
    std::vector<char*>  alias_ptrs;
    std::vector<std::string> addrs;     // 每个元素是一条原始地址字节
    std::vector<char*>  addr_ptrs;

    void Clear()
    {
        std::memset(&ent, 0, sizeof(ent));
        name.clear();
        aliases.clear();
        alias_ptrs.clear();
        addrs.clear();
        addr_ptrs.clear();
    }
};

static thread_local TlsHostentStorage tls_hostent;

/**
 * @brief 把 getaddrinfo 结果链填进 TLS hostent（调用线程执行，ai 为 libc 堆内存）
 *
 * ponytail: aliases 恒为空——getaddrinfo 只给 canonname，不给 /etc/hosts 别名列表；
 * 需要别名再加 gethostbyname_r 直通路径。
 */
static struct hostent* FillHostentFromAddrInfo(struct addrinfo* ai)
{
    auto& tls = tls_hostent;
    tls.Clear();

    for (auto* p = ai; p != nullptr; p = p->ai_next) {
        if (p->ai_family != AF_INET || p->ai_addrlen < sizeof(sockaddr_in))
            continue;
        const auto* sin = reinterpret_cast<const struct sockaddr_in*>(p->ai_addr);
        tls.addrs.emplace_back(reinterpret_cast<const char*>(&sin->sin_addr), sizeof(sin->sin_addr));
    }
    if (tls.addrs.empty())
        return nullptr;

    if (ai->ai_canonname != nullptr && ai->ai_canonname[0] != '\0') {
        tls.name = ai->ai_canonname;
    } else {
        // 数字地址无 canonname：与 glibc 一致，回退为点分地址串
        char buf[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, tls.addrs[0].data(), buf, sizeof(buf)) == nullptr)
            return nullptr;
        tls.name = buf;
    }

    tls.alias_ptrs.push_back(nullptr);
    for (auto& a : tls.addrs)
        tls.addr_ptrs.push_back(a.data());
    tls.addr_ptrs.push_back(nullptr);

    tls.ent.h_name = tls.name.data();
    tls.ent.h_aliases = tls.alias_ptrs.data();
    tls.ent.h_addrtype = AF_INET;
    tls.ent.h_length = sizeof(struct in_addr);
    tls.ent.h_addr_list = tls.addr_ptrs.data();
    return &tls.ent;
}

/**
 * @brief gethostbyaddr 结果的跨线程拷贝（#231）
 *
 * worker 线程上调 libc 原函数后，libc 静态缓冲随时会被本线程下次调用覆盖，
 * 且协程恢复执行的线程不确定，必须把内容拷进调用方栈上的这份结构，
 * 恢复后由调用线程再搬进自己的 TLS 存储。
 */
struct HostentCopy
{
    bool                ok{false};
    int                 h_err{0};
    int                 addrtype{0};
    int                 length{0};
    std::string         name;
    std::vector<std::string> aliases;
    std::vector<std::string> addrs;
};

static int GaiErrToHErrno(int gai)
{
    switch (gai) {
    case EAI_NONAME:      return HOST_NOT_FOUND;
    case EAI_AGAIN:       return TRY_AGAIN;
    case EAI_FAIL:        return NO_RECOVERY;
    default:              return NO_DATA;
    }
}

/**
 * @brief 协程内 getaddrinfo（#231）
 *
 * 结果 res/ret 在调用方协程栈上，挂起期间有效（worker 完成前协程不会返回）。
 * 入队失败（池已停止）时 work 不执行，返回预置的 EAI_FAIL。
 */
int Hook_GetAddrInfo(const char* node, const char* service, const struct addrinfo* req, struct addrinfo** res)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "must be in coroutine context");
    if (res == nullptr)
        return g_bbt_sys_hook_getaddrinfo_func(node, service, req, res);

    int ret = EAI_FAIL;
    *res = nullptr;
    DnsResolver::GetInstance()->Await([&ret, res, node, service, req]() {
        ret = g_bbt_sys_hook_getaddrinfo_func(node, service, req, res);
    });
    return ret;
}

/**
 * @brief 协程内 getnameinfo（#231）
 *
 * host/serv 是调用方缓冲，挂起期间有效；入队失败返回预置 EAI_FAIL。
 */
int Hook_GetNameInfo(const struct sockaddr* sa, socklen_t salen, char* host, socklen_t hostlen, char* serv, socklen_t servlen, int flags)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "must be in coroutine context");
    int ret = EAI_FAIL;
    DnsResolver::GetInstance()->Await([&ret, sa, salen, host, hostlen, serv, servlen, flags]() {
        ret = g_bbt_sys_hook_getnameinfo_func(sa, salen, host, hostlen, serv, servlen, flags);
    });
    return ret;
}

/**
 * @brief 协程内 gethostbyname（#231）
 *
 * 用 getaddrinfo(AF_INET) 实现（gethostbyname 本身只支持 IPv4 查询语义），
 * 结果填调用线程的 TLS hostent。失败按 glibc 口径写 h_errno。
 */
struct hostent* Hook_GetHostByName(const char* name)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "must be in coroutine context");

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;

    int gai = EAI_FAIL;
    struct addrinfo* ai = nullptr;
    DnsResolver::GetInstance()->Await([&gai, &ai, name, &hints]() {
        gai = g_bbt_sys_hook_getaddrinfo_func(name, nullptr, &hints, &ai);
    });

    if (gai != 0 || ai == nullptr) {
        if (ai != nullptr)
            ::freeaddrinfo(ai);
        h_errno = GaiErrToHErrno(gai);
        return nullptr;
    }

    struct hostent* he = FillHostentFromAddrInfo(ai);
    ::freeaddrinfo(ai);
    if (he == nullptr)
        h_errno = NO_ADDRESS;
    return he;
}

/**
 * @brief 协程内 gethostbyaddr（#231）
 *
 * worker 上调原函数后把 libc 静态缓冲内容拷进调用方栈（HostentCopy），
 * 恢复后由调用线程搬进 TLS hostent——禁止把 libc 静态指针跨线程返回。
 */
struct hostent* Hook_GetHostByAddr(const void* addr, socklen_t len, int type)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "must be in coroutine context");

    HostentCopy copy;
    DnsResolver::GetInstance()->Await([&copy, addr, len, type]() {
        struct hostent* he = g_bbt_sys_hook_gethostbyaddr_func(addr, len, type);
        copy.h_err = h_errno;
        if (he == nullptr)
            return;
        copy.ok = true;
        copy.addrtype = he->h_addrtype;
        copy.length = he->h_length;
        if (he->h_name != nullptr)
            copy.name = he->h_name;
        for (char** it = he->h_aliases; it != nullptr && *it != nullptr; ++it)
            copy.aliases.emplace_back(*it);
        for (char** it = he->h_addr_list; it != nullptr && *it != nullptr; ++it)
            copy.addrs.emplace_back(*it, static_cast<size_t>(he->h_length));
    });

    if (!copy.ok) {
        h_errno = copy.h_err != 0 ? copy.h_err : HOST_NOT_FOUND;
        return nullptr;
    }

    auto& tls = tls_hostent;
    tls.Clear();
    tls.name = std::move(copy.name);
    tls.aliases = std::move(copy.aliases);
    tls.addrs = std::move(copy.addrs);
    // string 数据在 tls 内不再变动，取指针后入 vector 是安全的
    for (auto& a : tls.aliases)
        tls.alias_ptrs.push_back(a.data());
    tls.alias_ptrs.push_back(nullptr);
    for (auto& a : tls.addrs)
        tls.addr_ptrs.push_back(a.data());
    tls.addr_ptrs.push_back(nullptr);

    tls.ent.h_name = tls.name.empty() ? nullptr : tls.name.data();
    tls.ent.h_aliases = tls.alias_ptrs.data();
    tls.ent.h_addrtype = copy.addrtype;
    tls.ent.h_length = copy.length;
    tls.ent.h_addr_list = tls.addr_ptrs.data();
    return &tls.ent;
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

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_poll_func(fds, nfds, timeout_ms);

    return bbt::coroutine::detail::Hook_Poll(fds, nfds, timeout_ms);
}

int select(int nfds, fd_set *read_fds, fd_set *write_fds, fd_set *except_fds, struct timeval *timeout)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_select_func(nfds, read_fds, write_fds, except_fds, timeout);

    return bbt::coroutine::detail::Hook_Select(nfds, read_fds, write_fds, except_fds, timeout);
}

int pselect(int nfds, fd_set *read_fds, fd_set *write_fds, fd_set *except_fds, const struct timespec *timeout, const sigset_t *sigmask)
{
    /* 非协程直通原函数（含 sigmask 语义） */
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_pselect_func(nfds, read_fds, write_fds, except_fds, timeout, sigmask);

    return bbt::coroutine::detail::Hook_PSelect(nfds, read_fds, write_fds, except_fds, timeout, sigmask);
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *req, struct addrinfo **res)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_getaddrinfo_func(node, service, req, res);

    return bbt::coroutine::detail::Hook_GetAddrInfo(node, service, req, res);
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_getnameinfo_func(sa, salen, host, hostlen, serv, servlen, flags);

    return bbt::coroutine::detail::Hook_GetNameInfo(sa, salen, host, hostlen, serv, servlen, flags);
}

struct hostent *gethostbyname(const char *name)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_gethostbyname_func(name);

    return bbt::coroutine::detail::Hook_GetHostByName(name);
}

struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return g_bbt_sys_hook_gethostbyaddr_func(addr, len, type);

    return bbt::coroutine::detail::Hook_GetHostByAddr(addr, len, type);
}
