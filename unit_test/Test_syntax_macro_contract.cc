#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>

#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

using namespace bbt::coroutine;

namespace
{

struct RuntimeConfigSnapshot
{
    size_t threads{0};
    size_t stack_size{0};
    bool stack_protect{false};
};

class ScopedSchedulerRuntime
{
public:
    explicit ScopedSchedulerRuntime(size_t threads)
    {
        auto* cfg = g_bbt_coroutine_config.get();
        m_snapshot.threads = cfg->m_cfg_static_thread_num;
        m_snapshot.stack_size = cfg->m_cfg_stack_size;
        m_snapshot.stack_protect = cfg->m_cfg_stack_protect;

        cfg->m_cfg_static_thread_num = threads;
        cfg->m_cfg_stack_size = 4096;
        cfg->m_cfg_stack_protect = false;
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    }

    ~ScopedSchedulerRuntime()
    {
        if (g_scheduler->IsRunning())
            g_scheduler->Stop();

        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = m_snapshot.threads;
        cfg->m_cfg_stack_size = m_snapshot.stack_size;
        cfg->m_cfg_stack_protect = m_snapshot.stack_protect;
    }

private:
    RuntimeConfigSnapshot m_snapshot;
};

}

BOOST_AUTO_TEST_SUITE(SyntaxMacroContractTest)

BOOST_AUTO_TEST_CASE(t_bbtco_noexcept_reports_registration_failure)
{
    bool succ = true;
    bbtco_noexcept(&succ) []() {};
    BOOST_CHECK(!succ);
}

BOOST_AUTO_TEST_CASE(t_bbtco_yield_is_noop_outside_coroutine)
{
    std::atomic_int value{1};
    bbtco_yield;
    BOOST_CHECK_EQUAL(value.load(), 1);
}

BOOST_AUTO_TEST_CASE(t_bbtco_sleep_suspends_coroutine_without_blocking_worker)
{
    ScopedSchedulerRuntime runtime(1);
    bbt::core::thread::CountDownLatch latch{2};
    std::atomic_int elapsed_ms{0};
    std::atomic_bool peer_ran{false};

    bbtco [&]() {
        auto begin = bbt::core::clock::gettime();
        bbtco_sleep(50);
        elapsed_ms.store(static_cast<int>(bbt::core::clock::gettime() - begin));
        latch.Down();
    };

    bbtco [&]() {
        peer_ran.store(true);
        latch.Down();
    };

    latch.Wait();
    BOOST_CHECK(peer_ran.load());
    BOOST_CHECK_GE(elapsed_ms.load(), 50);
}

BOOST_AUTO_TEST_CASE(t_bbtco_wait_for_and_event_helpers_follow_scheduler_path)
{
    ScopedSchedulerRuntime runtime(1);
    bbt::core::thread::CountDownLatch latch{2};
    int pipefd[2];
    BOOST_REQUIRE(::pipe(pipefd) == 0);
    std::atomic_bool readable_path{false};
    std::atomic_bool timeout_path{false};

    bbtco [&]() {
        bbtco_wait_for(pipefd[0], bbtco_emev_readable, 0);
        char value = 0;
        BOOST_REQUIRE_EQUAL(::read(pipefd[0], &value, 1), 1);
        readable_path.store(value == 'x');
        latch.Down();
    };

    bbtco [&]() {
        bbtco_sleep(20);
        BOOST_REQUIRE_EQUAL(::write(pipefd[1], "x", 1), 1);
    };

    auto event_succ = bbtco_ev_t(10) [&](int, short) {
        timeout_path.store(true);
        latch.Down();
    };

    BOOST_REQUIRE_EQUAL(event_succ, 0);
    latch.Wait();
    BOOST_CHECK(readable_path.load());
    BOOST_CHECK(timeout_path.load());

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

BOOST_AUTO_TEST_SUITE_END()