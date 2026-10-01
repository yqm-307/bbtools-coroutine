#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <mutex>

#include <bbt/coroutine/coroutine.hpp>

/* 进程寿命模型：runtime 只初始化一次，重复 Start 抛 std::logic_error。
 * 每个测试文件就是一个可执行，这里把用例内的 Start() 收敛为进程内一次初始化。 */
namespace
{
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, [](){
        bbt::coroutine::detail::Scheduler::GetInstance()->Start();
    });
}
}

BOOST_AUTO_TEST_SUITE(CoEventRegist)

BOOST_AUTO_TEST_CASE(t_begin)
{
    EnsureRuntime();
}

/* 外部事件注册 */
BOOST_AUTO_TEST_CASE(t_regist_event_timeout)
{
    bbt::core::thread::CountDownLatch l{1};
    auto begin = bbt::core::clock::gettime();

    bbtco_ev_t(100) [&](int fd, short event){
        BOOST_CHECK(bbt::core::clock::gettime() - begin >= 100);
        l.Down();
        return false;
    };

    l.Wait();
}

/* fd可读事件 */
BOOST_AUTO_TEST_CASE(t_regist_event_fd_ev_readable)
{
    bbt::core::thread::CountDownLatch l{1};
    const char msg[] = "hello";

    int fds[2];
    Assert(pipe(fds) == 0);
    int rfd = fds[0];
    int wfd = fds[1];

    bbtco_ev_r(fds[0]) [=, &l](int fd, short event){
        char buf[10];
        memset(buf, '\0', sizeof(buf));
        int size = ::read(fd, buf, sizeof(buf));
        BOOST_CHECK_EQUAL(size, strlen(msg));
        BOOST_CHECK_EQUAL(std::string(buf), std::string(msg));
        l.Down();
        return false;
    };

    BOOST_CHECK_EQUAL(::write(wfd, msg, strlen(msg)), strlen(msg));
    l.Wait();
    ::close(rfd);
    ::close(wfd);
}


/* fd可写事件 */
BOOST_AUTO_TEST_CASE(t_regist_event_fd_ev_writeable)
{
    bbt::core::thread::CountDownLatch l{1};
    const char msg[] = "hello";
    char buf[10] = {'0'};

    int fds[2];
    Assert(pipe(fds) == 0);
    int rfd = fds[0];
    int wfd = fds[1];

    bbtco_ev_w(fds[1]) [&](int fd, short event)
    {
        BOOST_CHECK_EQUAL(::write(fd, msg, strlen(msg)), strlen(msg));
    };

    BOOST_CHECK_EQUAL(::read(rfd, buf, sizeof(msg)), strlen(msg));
    BOOST_CHECK(memcmp(buf, msg, strlen(msg)) == 0);
    ::close(rfd);
    ::close(wfd);
}

BOOST_AUTO_TEST_CASE(t_regist_event_with_copool)
{
    bbt::core::thread::CountDownLatch l{1};

    auto copool = bbtco_make_copool(10);
    auto cocond = bbtco_make_cocond();
    auto chan = bbt::co::sync::Chan<int, 10>();

    auto succ = bbtco_ev_t_with_copool(10, copool) [&](auto, auto){
        for (int i = 0; i < 100; ++i)
        {
            chan << i;
        }
    };
    Assert(succ == 0);

    bbtco [&](){
        while (true)
        {
            int val = 0;
            chan >> val;
            // printf("read: %d\n", val);
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

    bbtco_ev_rt(fds[1], 100) [&](int fd, short event)
    {
        BOOST_CHECK_GE(bbt::core::clock::gettime() - 100, start_time);
        l.Down();
    };

    l.Wait();
    ::close(rfd);
    ::close(wfd);
}

BOOST_AUTO_TEST_CASE(t_end)
{
}

BOOST_AUTO_TEST_SUITE_END()