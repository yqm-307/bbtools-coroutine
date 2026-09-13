#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

namespace
{
struct ConfigSnapshot
{
    size_t threads{0};
    size_t stack_size{0};
    bool stack_protect{false};
    bool saved{false};
} g_cfg;
}

BOOST_AUTO_TEST_SUITE(SchedulerApiTest)

BOOST_AUTO_TEST_CASE(t_singleton_is_g_scheduler)
{
    BOOST_CHECK_EQUAL(g_scheduler.get(), Scheduler::GetInstance().get());
}

BOOST_AUTO_TEST_CASE(t_start_submit_stop_restart)
{
    auto* cfg = g_bbt_coroutine_config.get();
    if (!g_cfg.saved) {
        g_cfg.threads = cfg->m_cfg_static_thread_num;
        g_cfg.stack_size = cfg->m_cfg_stack_size;
        g_cfg.stack_protect = cfg->m_cfg_stack_protect;
        g_cfg.saved = true;
    }
    cfg->m_cfg_static_thread_num = 1;
    cfg->m_cfg_stack_protect = false;

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    BOOST_REQUIRE(g_scheduler->IsRunning());

    bbt::core::thread::CountDownLatch done{1};
    g_scheduler->RegistCoroutineTask([&]() { done.Down(); });
    done.Wait();

    g_scheduler->Stop();
    BOOST_CHECK(!g_scheduler->IsRunning());

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    BOOST_REQUIRE(g_scheduler->IsRunning());
    bbt::core::thread::CountDownLatch done2{1};
    bool succ = false;
    g_scheduler->RegistCoroutineTask([&]() { done2.Down(); }, succ);
    BOOST_CHECK(succ);
    done2.Wait();
    g_scheduler->Stop();

    if (g_cfg.saved) {
        cfg->m_cfg_static_thread_num = g_cfg.threads;
        cfg->m_cfg_stack_size = g_cfg.stack_size;
        cfg->m_cfg_stack_protect = g_cfg.stack_protect;
        g_cfg.saved = false;
    }
}

BOOST_AUTO_TEST_CASE(t_noloop_looponce)
{
    auto* cfg = g_bbt_coroutine_config.get();
    const auto threads = cfg->m_cfg_static_thread_num;
    const auto stack = cfg->m_cfg_stack_size;
    const auto protect = cfg->m_cfg_stack_protect;
    cfg->m_cfg_static_thread_num = 1;
    cfg->m_cfg_stack_protect = false;

    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    BOOST_REQUIRE(g_scheduler->IsRunning());
    g_scheduler->LoopOnce();

    bbt::core::thread::CountDownLatch done{1};
    g_scheduler->RegistCoroutineTask([&]() { done.Down(); });
    done.Wait();

    g_scheduler->Stop();
    BOOST_CHECK(!g_scheduler->IsRunning());

    cfg->m_cfg_static_thread_num = threads;
    cfg->m_cfg_stack_size = stack;
    cfg->m_cfg_stack_protect = protect;
}

BOOST_AUTO_TEST_SUITE_END()
