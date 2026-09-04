#pragma once
// #232 hook 行为契约测试基座：只提供契约用例实际用到的最小原语。
// WHY: 新 hook 的契约用例应当是复制一个 BOOST_AUTO_TEST_CASE 就能写的程度，
// 所以这里刻意不做函数指针模板/宏框架，避免为不存在的复用抽象买单。

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/thread/Lock.hpp>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace hook_contract
{

/**
 * @brief fd 的 RAII 守卫，析构时 close
 *
 * WHY: 契约用例大量走失败断言路径，异常/提前返回时裸 fd 会泄漏并污染后续用例。
 */
class FdGuard
{
public:
    explicit FdGuard(int fd = -1): m_fd(fd) {}
    ~FdGuard() { if (m_fd >= 0) ::close(m_fd); }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    int Get() const { return m_fd; }
private:
    int m_fd;
};

/**
 * @brief socketpair 包装，析构自动关闭两端
 *
 * WHY: socketpair 本身不在 hook 列表内，得到的是原生（阻塞）fd；
 * 协程用例必须先 SetNonblock，否则 hook 内部的原生 read 会阻塞执行线程而不是走挂起路径。
 */
class SocketPair
{
public:
    SocketPair() { ::socketpair(AF_UNIX, SOCK_STREAM, 0, m_fds); }
    ~SocketPair() { for (int& fd : m_fds) if (fd >= 0) ::close(fd); }
    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;
    int operator[](int i) const { return m_fds[i]; }
    bool Ok() const { return m_fds[0] >= 0 && m_fds[1] >= 0; }
private:
    int m_fds[2]{-1, -1};
};

inline bool SetNonblock(int fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline bool IsNonblock(int fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && (flags & O_NONBLOCK) != 0;
}

/**
 * @brief 把 fd bind 到 loopback 的 port 0 并取回实际地址
 *
 * WHY: 契约测试禁止写死端口（并发/重跑会撞端口），一律由内核分配后 getsockname 回读。
 */
inline bool BindLoopbackPort0(int fd, sockaddr_in* addr)
{
    std::memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr->sin_port = 0;
    if (::bind(fd, (struct sockaddr*)addr, sizeof(*addr)) != 0)
        return false;
    socklen_t len = sizeof(*addr);
    return ::getsockname(fd, (struct sockaddr*)addr, &len) == 0;
}

/**
 * @brief 在协程上下文执行 fn 并等待其结束
 *
 * 协程体跑在调度器工作线程上（EnableUseCo()==true），::socket 等调用命中 hook；
 * 测试线程自身 EnableUseCo()==false，即非协程直通环境。
 * WHY 捕获异常带回：Boost 断言在协程栈上抛出若无人接住，latch 永不释放，
 * 用例表现为挂死到 CTest TIMEOUT，无法定位；rethrow 回测试线程后按正常断言失败报告。
 */
inline void RunInCo(const std::function<void()>& fn)
{
    bbt::core::thread::CountDownLatch latch{1};
    std::exception_ptr eptr;

    bbtco [&]() {
        try { fn(); }
        catch (...) { eptr = std::current_exception(); }
        latch.Down();
    };

    latch.Wait();
    if (eptr)
        std::rethrow_exception(eptr);
}

/**
 * @brief 为当前线程构造一次 EINTR：SIGALRM 到期打断阻塞系统调用
 *
 * WHY: handler 用 sigaction 注册且不带 SA_RESTART，libc 才会让被中断的 read
 * 返回 -1/EINTR 而不是自动重启；析构时恢复原 handler 并取消定时器，不泄漏信号状态。
 */
class EintrArm
{
public:
    explicit EintrArm(unsigned int usec)
    {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = &NopHandler;  // 空处理、无 SA_RESTART
        ::sigaction(SIGALRM, &sa, &m_old);
        ::ualarm(usec, 0);
    }
    ~EintrArm()
    {
        ::ualarm(0, 0);
        ::sigaction(SIGALRM, &m_old, nullptr);
    }
    EintrArm(const EintrArm&) = delete;
    EintrArm& operator=(const EintrArm&) = delete;
private:
    static void NopHandler(int) {}
    struct sigaction m_old;
};

} // namespace hook_contract
