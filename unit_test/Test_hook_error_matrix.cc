#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

// #262 Hook POSIX 错误、EOF、关闭与降级矩阵。
// 探针：EOF/EPIPE/ECONNRESET 在协程内保持原生 errno；等待中 fd 被关闭必须唤醒，
// 不得永久挂起（唤醒后 syscall 返回 -1/EBADF）。

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(HookErrorMatrixTest)

namespace
{

struct CoFixture
{
    CoFixture()
    {
        // 写已关 socket 默认触发 SIGPIPE 杀进程；屏蔽后 write 返回 -1/EPIPE，
        // 才能验证 Hook 保持 errno 的原生语义（POSIX 语义测试标准做法）。
        m_old_pipe = std::signal(SIGPIPE, SIG_IGN);
        auto* cfg = g_bbt_coroutine_config.get();
        m_threads = cfg->m_cfg_static_thread_num;
        m_stack = cfg->m_cfg_stack_size;
        m_protect = cfg->m_cfg_stack_protect;
        cfg->m_cfg_static_thread_num = 1;
        cfg->m_cfg_stack_protect = false;
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    }
    ~CoFixture()
    {
        std::signal(SIGPIPE, m_old_pipe);
        g_scheduler->Stop();
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = m_threads;
        cfg->m_cfg_stack_size = m_stack;
        cfg->m_cfg_stack_protect = m_protect;
    }
    int m_threads;
    size_t m_stack;
    bool m_protect;
    void (*m_old_pipe)(int){SIG_DFL};
};

template <typename Fn>
void RunInCo(Fn fn)
{
    bbt::core::thread::CountDownLatch done{1};
    std::exception_ptr eptr;
    bbtco [&]() {
        try { fn(); }
        catch (...) { eptr = std::current_exception(); }
        done.Down();
    };
    done.Wait();
    if (eptr)
        std::rethrow_exception(eptr);
}

// 带 deadline 的 latch 等待（同 Test_chan：CI 旧 bbtools-core 的 WaitTimeout 不可靠）
bool WaitLatch(bbt::core::thread::CountDownLatch& latch, int ms)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (latch.WaitTimeout(1) == 0)
            return true;
    }
    return latch.WaitTimeout(0) == 0;
}

} // namespace

// EOF：对端 close 后 read 返回 0（原生 EOF 语义，不转错误）
BOOST_AUTO_TEST_CASE(t_eof_read_returns_zero)
{
    CoFixture fx;
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    RunInCo([&]() {
        BOOST_REQUIRE_EQUAL(::close(fds[1]), 0);
        fds[1] = -1;
        char buf[4];
        const ssize_t r = ::read(fds[0], buf, sizeof(buf));
        BOOST_CHECK_EQUAL(r, 0);
    });
    if (fds[1] >= 0) ::close(fds[1]);
    ::close(fds[0]);
}

// EPIPE：对端关闭后 write 返回 -1/EPIPE（AF_UNIX 首次即 EPIPE）
BOOST_AUTO_TEST_CASE(t_epipe_write_errno_preserved)
{
    CoFixture fx;
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    RunInCo([&]() {
        BOOST_REQUIRE_EQUAL(::close(fds[1]), 0);
        fds[1] = -1;
        char c = 'x';
        errno = 0;
        const ssize_t r = ::write(fds[0], &c, 1);
        BOOST_CHECK_EQUAL(r, -1);
        BOOST_CHECK(errno == EPIPE || errno == ECONNRESET);
    });
    if (fds[1] >= 0) ::close(fds[1]);
    ::close(fds[0]);
}

// ECONNRESET：对端 RST（SO_LINGER 0 close）后 recv 返回 -1/ECONNRESET
BOOST_AUTO_TEST_CASE(t_econnreset_recv_errno_preserved)
{
    CoFixture fx;
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(srv >= 0);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    BOOST_REQUIRE_EQUAL(::bind(srv, (sockaddr*)&addr, sizeof(addr)), 0);
    BOOST_REQUIRE_EQUAL(::listen(srv, 1), 0);
    socklen_t alen = sizeof(addr);
    BOOST_REQUIRE_EQUAL(::getsockname(srv, (sockaddr*)&addr, &alen), 0);

    int cli = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(cli >= 0);
    // 非协程线程 blocking connect 到本机 backlog，立即成功
    BOOST_REQUIRE_EQUAL(::connect(cli, (sockaddr*)&addr, sizeof(addr)), 0);
    int acc = ::accept(srv, nullptr, nullptr);
    BOOST_REQUIRE(acc >= 0);

    const struct linger lg{1, 0};
    BOOST_REQUIRE_EQUAL(::setsockopt(acc, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)), 0);
    BOOST_REQUIRE_EQUAL(::close(acc), 0);   // RST 发给 cli

    RunInCo([&]() {
        char buf[4];
        errno = 0;
        const ssize_t r = ::recv(cli, buf, sizeof(buf), 0);
        BOOST_CHECK_EQUAL(r, -1);
        BOOST_CHECK_EQUAL(errno, ECONNRESET);
    });
    ::close(cli);
    ::close(srv);
}

// 等待中 fd 被关闭：协程必须被唤醒并返回 -1/EBADF，不得永久挂起
BOOST_AUTO_TEST_CASE(t_close_during_wait_wakes_with_ebadf)
{
    CoFixture fx;
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    bbt::core::thread::CountDownLatch done{1};
    std::atomic<ssize_t> result{12345};
    std::atomic_int errcode{0};

    bbtco [&]() {
        char buf[1];
        errno = 0;
        const ssize_t r = ::read(fds[0], buf, 1);
        result.store(r);
        errcode.store(errno);
        done.Down();
    };
    bbtco [&]() {
        bbtco_sleep(20);
        BOOST_CHECK_EQUAL(::close(fds[0]), 0);
    };

    const bool ok = WaitLatch(done, 3000);
    BOOST_CHECK_MESSAGE(ok, "close during wait did not wake the coroutine");
    if (ok) {
        BOOST_CHECK_EQUAL(result.load(), -1);
        BOOST_CHECK_EQUAL(errcode.load(), EBADF);
    }
    if (fds[0] >= 0) ::close(fds[0]);
    ::close(fds[1]);
}

BOOST_AUTO_TEST_SUITE_END()
