#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(MacroWrapTest)

BOOST_AUTO_TEST_CASE(t_bbtco_family_registers)
{
    auto* cfg = g_bbt_coroutine_config.get();
    const auto threads = cfg->m_cfg_static_thread_num;
    const auto stack = cfg->m_cfg_stack_size;
    const auto protect = cfg->m_cfg_stack_protect;
    cfg->m_cfg_static_thread_num = 1;
    cfg->m_cfg_stack_size = 4096;
    cfg->m_cfg_stack_protect = false;

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    bbt::core::thread::CountDownLatch done{3};
    std::atomic_int n{0};

    bbtco [&]() { n.fetch_add(1); done.Down(); };
    bbtco_desc("ignored") [&]() { n.fetch_add(1); done.Down(); };
    bool succ = false;
    bbtco_noexcept(&succ) [&]() { n.fetch_add(1); done.Down(); };

    done.Wait();
    BOOST_CHECK(succ);
    BOOST_CHECK_EQUAL(n.load(), 3);

    g_scheduler->Stop();
    cfg->m_cfg_static_thread_num = threads;
    cfg->m_cfg_stack_size = stack;
    cfg->m_cfg_stack_protect = protect;
}

BOOST_AUTO_TEST_CASE(t_yield_and_sleep_are_wrappers)
{
    auto* cfg = g_bbt_coroutine_config.get();
    const auto threads = cfg->m_cfg_static_thread_num;
    const auto stack = cfg->m_cfg_stack_size;
    const auto protect = cfg->m_cfg_stack_protect;
    cfg->m_cfg_static_thread_num = 1;
    cfg->m_cfg_stack_size = 4096;
    cfg->m_cfg_stack_protect = false;

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    bbt::core::thread::CountDownLatch done{1};
    const auto t0 = std::chrono::steady_clock::now();

    bbtco [&]() {
        bbtco_yield;
        bbtco_sleep(20);
        done.Down();
    };

    done.Wait();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    BOOST_CHECK_GE(ms, 15);

    g_scheduler->Stop();
    cfg->m_cfg_static_thread_num = threads;
    cfg->m_cfg_stack_size = stack;
    cfg->m_cfg_stack_protect = protect;
}

BOOST_AUTO_TEST_SUITE_END()
