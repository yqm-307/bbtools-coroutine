#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(HookBlockingFdTest)

BOOST_AUTO_TEST_CASE(t_external_blocking_read_yields_and_restores_flags)
{
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    const int fl0 = ::fcntl(fds[0], F_GETFL, 0);
    BOOST_REQUIRE(fl0 >= 0);
    BOOST_REQUIRE_EQUAL(fl0 & O_NONBLOCK, 0);

    auto* cfg = g_bbt_coroutine_config.get();
    const auto threads = cfg->m_cfg_static_thread_num;
    const auto stack = cfg->m_cfg_stack_size;
    const auto protect = cfg->m_cfg_stack_protect;
    cfg->m_cfg_static_thread_num = 1;
    cfg->m_cfg_stack_size = 4096;
    cfg->m_cfg_stack_protect = false;

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int ticker{0};
    std::atomic_int n{0};

    bbtco [&]() {
        while (n.load() == 0) {
            ticker.fetch_add(1);
            bbtco_yield;
        }
        done.Down();
    };
    bbtco [&]() {
        char buf[1];
        const ssize_t r = ::read(fds[0], buf, 1);
        BOOST_CHECK_EQUAL(r, 1);
        n.store(1);
    };
    bbtco [&]() {
        bbtco_sleep(20);
        char c = 'x';
        BOOST_CHECK_EQUAL(::write(fds[1], &c, 1), 1);
    };

    done.Wait();
    BOOST_CHECK_GT(ticker.load(), 0);

    const int fl1 = ::fcntl(fds[0], F_GETFL, 0);
    BOOST_CHECK_EQUAL(fl1 & O_NONBLOCK, 0);

    g_scheduler->Stop();
    cfg->m_cfg_static_thread_num = threads;
    cfg->m_cfg_stack_size = stack;
    cfg->m_cfg_stack_protect = protect;
    ::close(fds[0]);
    ::close(fds[1]);
}

BOOST_AUTO_TEST_SUITE_END()
