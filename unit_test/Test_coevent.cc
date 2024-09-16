#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>

BOOST_AUTO_TEST_SUITE(CoEventRegist)

BOOST_AUTO_TEST_CASE(t_begin)
{
    g_scheduler->Start();
}

/* 外部事件注册 */
BOOST_AUTO_TEST_CASE(t_regist_event_timeout)
{
    bbt::thread::CountDownLatch l{1};
    auto begin = bbt::clock::gettime();

    bbtco_ev_t(100) [&](int fd, short event){
        BOOST_CHECK(bbt::clock::gettime() - begin >= 100);
        l.Down();
        return false;
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_regist_event_fd_ev_readable)
{
    bbt::thread::CountDownLatch l{1};
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

BOOST_AUTO_TEST_CASE(t_regist_event_persist)
{
    bbt::thread::CountDownLatch l{1};

    int i = 0;

    __bbtco_event_regist_ex(-1, bbtco_emev_persist, 10) [&](auto, short event){
        if (++i < 5)
            return true;
        
        l.Down();
        return false;
    }; 

    l.Wait();
    BOOST_CHECK_EQUAL(i, 5);
}

BOOST_AUTO_TEST_CASE(t_regist_event_with_copool)
{
    bbt::thread::CountDownLatch l{1};
    int i = 0;
    auto copool = bbtco_make_copool(10);

    __bbtco_event_regist_with_copool_ex(-1, bbtco_emev_persist, 10, copool)
    [&](auto, short event){
        if (++i < 5)
            return true;
        
        l.Down();
        return false;
    };

    l.Wait();
    copool->Release();
    BOOST_CHECK_EQUAL(i, 5);
}

BOOST_AUTO_TEST_CASE(t_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()