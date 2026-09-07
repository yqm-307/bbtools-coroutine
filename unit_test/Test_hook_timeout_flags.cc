#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

// #261 Hook flags 与 socket timeout 语义保持。
// 铁律：① MSG_DONTWAIT 在 send/recv/sendto/recvfrom 也立即返回 EAGAIN（不只 msg 家族）
// ② SO_RCVTIMEO/SO_SNDTIMEO 到期返回 -1/EAGAIN，不静默变无限协程等待
// ③ 等待期间 worker 不被占死（ticker 推进）。

#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(HookTimeoutFlagsTest)

namespace
{

struct CoFixture
{
    CoFixture()
    {
        auto* cfg = g_bbt_coroutine_config.get();
        m_threads = cfg->m_cfg_static_thread_num;
        m_stack = cfg->m_cfg_stack_size;
        m_protect = cfg->m_cfg_stack_protect;
        cfg->m_cfg_static_thread_num = 1;
        cfg->m_cfg_stack_size = 4096;
        cfg->m_cfg_stack_protect = false;
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    }
    ~CoFixture()
    {
        g_scheduler->Stop();
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = m_threads;
        cfg->m_cfg_stack_size = m_stack;
        cfg->m_cfg_stack_protect = m_protect;
    }
    int m_threads;
    size_t m_stack;
    bool m_protect;
};

// 在协程内跑 fn，同时一个 ticker 协程自旋证明 worker 未被占死；
// done 由 fn 置位后 ticker 退出。返回 ticker 次数。
template <typename Fn>
int RunWithTicker(Fn fn)
{
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int ticker{0};
    std::atomic_bool finished{false};

    bbtco [&]() {
        while (!finished.load()) {
            ticker.fetch_add(1);
            bbtco_yield;
        }
        done.Down();
    };
    bbtco [&]() {
        fn();
        finished.store(true);
    };

    done.Wait();
    return ticker.load();
}

int MakeBlockingPair(int fds[2])
{
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        return -1;
    int fl = ::fcntl(fds[0], F_GETFL, 0);
    ::fcntl(fds[0], F_SETFL, fl & ~O_NONBLOCK);
    fl = ::fcntl(fds[1], F_GETFL, 0);
    ::fcntl(fds[1], F_SETFL, fl & ~O_NONBLOCK);
    return 0;
}

// 裸 syscall 绕过 hook 灌满发送缓冲；SO_SNDTIMEO 让写有界，不会卡死测试线程
void FillSocketBuffer(int fd)
{
    const struct timeval fill_tv{0, 100 * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &fill_tv, sizeof(fill_tv));
    std::vector<char> big(64 * 1024, 'x');
    while (::syscall(SYS_write, fd, big.data(), big.size()) > 0) {}
}

} // namespace

// MSG_DONTWAIT：recv 空 socketpair 立即 -1/EAGAIN，不挂起
BOOST_AUTO_TEST_CASE(t_msg_dontwait_recv_immediate)
{
    CoFixture fx;
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(MakeBlockingPair(fds), 0);

    const int ticks = RunWithTicker([&]() {
        char buf[1];
        errno = 0;
        const ssize_t r = ::recv(fds[0], buf, 1, MSG_DONTWAIT);
        BOOST_CHECK_EQUAL(r, -1);
        BOOST_CHECK_EQUAL(errno, EAGAIN);
    });
    BOOST_CHECK_GT(ticks, 0);
    ::close(fds[0]);
    ::close(fds[1]);
}

// MSG_DONTWAIT：sendto 满 socketpair 立即 -1/EAGAIN，不挂起
BOOST_AUTO_TEST_CASE(t_msg_dontwait_sendto_immediate)
{
    CoFixture fx;
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(MakeBlockingPair(fds), 0);
    FillSocketBuffer(fds[1]);

    const int ticks = RunWithTicker([&]() {
        char c = 'y';
        errno = 0;
        const ssize_t r = ::sendto(fds[1], &c, 1, MSG_DONTWAIT, nullptr, 0);
        BOOST_CHECK_EQUAL(r, -1);
        BOOST_CHECK_EQUAL(errno, EAGAIN);
    });
    BOOST_CHECK_GT(ticks, 0);
    ::close(fds[0]);
    ::close(fds[1]);
}

// SO_RCVTIMEO：空 socketpair recv 到期返回 -1/EAGAIN，等待有界
BOOST_AUTO_TEST_CASE(t_rcvtimeo_bounded_eagain)
{
    CoFixture fx;
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(MakeBlockingPair(fds), 0);
    const struct timeval tv{0, 50 * 1000};
    BOOST_REQUIRE_EQUAL(::setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0);

    const auto t0 = std::chrono::steady_clock::now();
    const int ticks = RunWithTicker([&]() {
        char buf[1];
        errno = 0;
        const ssize_t r = ::recv(fds[0], buf, 1, 0);
        BOOST_CHECK_EQUAL(r, -1);
        BOOST_CHECK_EQUAL(errno, EAGAIN);
    });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    BOOST_CHECK_GE(ms, 40);     // 确实等过，不是立即假成功
    BOOST_CHECK_LT(ms, 5000);   // 有界返回，不是无限等待
    BOOST_CHECK_GT(ticks, 0);
    ::close(fds[0]);
    ::close(fds[1]);
}

// SO_SNDTIMEO：满缓冲 send 到期返回 -1/EAGAIN，等待有界
BOOST_AUTO_TEST_CASE(t_sndtimeo_bounded_eagain)
{
    CoFixture fx;
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(MakeBlockingPair(fds), 0);
    FillSocketBuffer(fds[1]);
    const struct timeval tv{0, 50 * 1000};
    BOOST_REQUIRE_EQUAL(::setsockopt(fds[1], SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)), 0);

    const auto t0 = std::chrono::steady_clock::now();
    const int ticks = RunWithTicker([&]() {
        char c = 'y';
        errno = 0;
        const ssize_t r = ::send(fds[1], &c, 1, 0);
        BOOST_CHECK_EQUAL(r, -1);
        BOOST_CHECK_EQUAL(errno, EAGAIN);
    });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    BOOST_CHECK_GE(ms, 40);
    BOOST_CHECK_LT(ms, 5000);
    BOOST_CHECK_GT(ticks, 0);
    ::close(fds[0]);
    ::close(fds[1]);
}

// SO_RCVTIMEO 未到期数据到达：正常返回数据（超时不吞就绪路径）
BOOST_AUTO_TEST_CASE(t_rcvtimeo_data_arrives_returns_data)
{
    CoFixture fx;
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(MakeBlockingPair(fds), 0);
    const struct timeval tv{0, 500 * 1000};
    BOOST_REQUIRE_EQUAL(::setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0);

    const int ticks = RunWithTicker([&]() {
        bbtco [&]() {
            bbtco_sleep(20);
            char c = 'x';
            BOOST_CHECK_EQUAL(::syscall(SYS_write, fds[1], &c, 1), 1);
        };
        char buf[1];
        const ssize_t r = ::recv(fds[0], buf, 1, 0);
        BOOST_CHECK_EQUAL(r, 1);
        BOOST_CHECK_EQUAL(buf[0], 'x');
    });
    BOOST_CHECK_GT(ticks, 0);
    ::close(fds[0]);
    ::close(fds[1]);
}

// SO_RCVTIMEO 对 read() 同样生效（Linux socket(7)：read→sock_recvmsg）
BOOST_AUTO_TEST_CASE(t_rcvtimeo_applies_to_read)
{
    CoFixture fx;
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(MakeBlockingPair(fds), 0);
    const struct timeval tv{0, 50 * 1000};
    BOOST_REQUIRE_EQUAL(::setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0);

    const auto t0 = std::chrono::steady_clock::now();
    const int ticks = RunWithTicker([&]() {
        char buf[1];
        errno = 0;
        const ssize_t r = ::read(fds[0], buf, 1);
        BOOST_CHECK_EQUAL(r, -1);
        BOOST_CHECK_EQUAL(errno, EAGAIN);
    });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    BOOST_CHECK_GE(ms, 40);
    BOOST_CHECK_LT(ms, 5000);
    BOOST_CHECK_GT(ticks, 0);
    ::close(fds[0]);
    ::close(fds[1]);
}

// 常规文件不受影响：无 SO_*TIMEO（getsockopt 失败）→ 正常读取，不假超时
BOOST_AUTO_TEST_CASE(t_regular_file_read_unaffected)
{
    CoFixture fx;
    char path[] = "/tmp/bbtools_261_XXXXXX";
    const int tf = ::mkstemp(path);
    BOOST_REQUIRE(tf >= 0);
    BOOST_REQUIRE_EQUAL(::write(tf, "ab", 2), 2);
    BOOST_REQUIRE_EQUAL(::lseek(tf, 0, SEEK_SET), 0);

    const int ticks = RunWithTicker([&]() {
        char buf[2];
        BOOST_CHECK_EQUAL(::read(tf, buf, 2), 2);
    });
    BOOST_CHECK_GT(ticks, 0);
    ::close(tf);
    ::unlink(path);
}

BOOST_AUTO_TEST_SUITE_END()
