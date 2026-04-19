#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

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

BOOST_AUTO_TEST_SUITE(SchedulerShutdownTest)

BOOST_AUTO_TEST_CASE(t_scheduler_stop_wakes_idle_processers)
{
    ScopedSchedulerRuntime runtime(2);

    auto begin = std::chrono::steady_clock::now();
    g_scheduler->Stop();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);

    BOOST_CHECK_LE(elapsed.count(), 1000);
    BOOST_CHECK(!g_scheduler->IsRunning());
}

BOOST_AUTO_TEST_CASE(t_scheduler_stop_handles_residual_global_runnable)
{
    ScopedSchedulerRuntime runtime(1);
    std::atomic_bool started{false};
    std::atomic_int executed{0};

    g_scheduler->RegistCoroutineTask([&]() {
        started.store(true);
        bbtco_sleep(200);
    });

    for (int i = 0; i < 64; ++i) {
        g_scheduler->RegistCoroutineTask([&]() {
            executed.fetch_add(1);
        });
    }

    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto begin = std::chrono::steady_clock::now();
    g_scheduler->Stop();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);

    BOOST_CHECK_LE(elapsed.count(), 1000);
    BOOST_CHECK_LT(executed.load(), 64);
}

BOOST_AUTO_TEST_CASE(t_scheduler_stop_rejects_worker_context_call)
{
    ScopedSchedulerRuntime runtime(1);
    std::atomic_bool threw{false};
    bbt::core::thread::CountDownLatch latch{1};

    g_scheduler->RegistCoroutineTask([&]() {
        try {
            g_scheduler->Stop();
        }
        catch (const std::runtime_error&) {
            threw.store(true);
        }

        latch.Down();
    });

    latch.Wait();
    BOOST_CHECK(threw.load());
    BOOST_CHECK(g_scheduler->IsRunning());
}

BOOST_AUTO_TEST_SUITE_END()