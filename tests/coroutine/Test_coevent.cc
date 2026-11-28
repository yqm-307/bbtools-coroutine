#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <atomic>
#include <mutex>

#include <bbt/coroutine/coroutine.hpp>

/**
 * 进程寿命模型：runtime 只初始化一次，重复 Start 抛 std::logic_error。
 * 每个测试文件就是一个可执行，这里把用例内的 Start() 收敛为进程内一次初始化。
 */
namespace {
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, []() { bbt::coroutine::detail::Scheduler::GetInstance()->Start(); });
}
} // namespace

BOOST_AUTO_TEST_SUITE(CoEventRegist)

BOOST_AUTO_TEST_CASE(t_begin)
{
    EnsureRuntime();
}

/**
 * 外部事件注册
 */
BOOST_AUTO_TEST_CASE(t_regist_event_timeout)
{
    bbt::core::thread::CountDownLatch l{1};
    auto begin = bbt::core::clock::gettime();
    std::atomic_bool ok{false};

    bbtco_ev_t(100) [&](int fd, short event) {
        ok.store(bbt::core::clock::gettime() - begin >= 100, std::memory_order_release);
        l.Down();
        return false;
    };

    l.Wait();
    BOOST_CHECK(ok.load(std::memory_order_acquire));
}

/**
 * fd可读事件
 */
BOOST_AUTO_TEST_CASE(t_regist_event_fd_ev_readable)
{
    bbt::core::thread::CountDownLatch l{1};
    const char msg[] = "hello";
    std::atomic_bool ok{false};

    int fds[2];
    Assert(pipe(fds) == 0);
    int rfd = fds[0];
    int wfd = fds[1];

    bbtco_ev_r(fds[0]) [=, &l, &ok](int fd, short event) {
        char buf[10];
        memset(buf, '\0', sizeof(buf));
        int size = ::read(fd, buf, sizeof(buf));
        ok.store(size == static_cast<int>(strlen(msg)) && std::string(buf) == msg, std::memory_order_release);
        l.Down();
        return false;
    };

    BOOST_CHECK_EQUAL(::write(wfd, msg, strlen(msg)), strlen(msg));
    l.Wait();
    BOOST_CHECK(ok.load(std::memory_order_acquire));
    ::close(rfd);
    ::close(wfd);
}

/**
 * fd可写事件
 */
BOOST_AUTO_TEST_CASE(t_regist_event_fd_ev_writeable)
{
    bbt::core::thread::CountDownLatch l{1};
    const char msg[] = "hello";
    char buf[10] = {'0'};
    std::atomic<ssize_t> callback_write{-1};

    int fds[2];
    Assert(pipe(fds) == 0);
    int rfd = fds[0];
    int wfd = fds[1];

    bbtco_ev_w(fds[1]) [&, &callback_write](int fd, short event) {
        callback_write.store(::write(fd, msg, strlen(msg)), std::memory_order_release);
    };

    BOOST_CHECK_EQUAL(::read(rfd, buf, sizeof(msg)), strlen(msg));
    BOOST_CHECK(memcmp(buf, msg, strlen(msg)) == 0);
    BOOST_CHECK_EQUAL(callback_write.load(std::memory_order_acquire), static_cast<ssize_t>(strlen(msg)));
    ::close(rfd);
    ::close(wfd);
}

BOOST_AUTO_TEST_CASE(t_regist_event_with_copool)
{
    bbt::core::thread::CountDownLatch l{1};

    auto copool = bbtco_make_copool(10);
    auto cocond = bbtco_make_cocond();
    auto chan = bbt::co::sync::Chan<int, 10>();

    auto succ = bbtco_ev_t_with_copool(10, copool) [&](auto, auto) {
        for (int i = 0; i < 100; ++i) {
            chan << i;
        }
    };
    Assert(succ == 0);

    bbtco [&]() {
        while (true) {
            int val = 0;
            chan >> val;

            /**
             * printf("read: %d\n", val);
             */
            if (val == 99)
                break;
        }

        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_regist_event_fd_timeout)
{
    bbt::core::thread::CountDownLatch l{1};

    int fds[2];
    Assert(pipe(fds) == 0);
    int rfd = fds[0];
    int wfd = fds[1];
    auto start_time = bbt::core::clock::gettime();
    std::atomic_bool ok{false};

    bbtco_ev_rt(fds[1], 100) [&](int fd, short event) {
        ok.store(bbt::core::clock::gettime() - start_time >= 100, std::memory_order_release);
        l.Down();
    };

    l.Wait();
    BOOST_CHECK(ok.load(std::memory_order_acquire));
    ::close(rfd);
    ::close(wfd);
}

BOOST_AUTO_TEST_CASE(t_end) {}

BOOST_AUTO_TEST_SUITE_END()