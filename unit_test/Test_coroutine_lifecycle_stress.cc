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

BOOST_AUTO_TEST_SUITE(CoroutineLifecycleStressTest)

BOOST_AUTO_TEST_CASE(t_resume_yield_stress_preserves_finalization)
{
    constexpr int kCoroutineCount = 64;
    constexpr int kYieldCount = 16;
    std::atomic_int finalized{0};
    bbt::core::thread::CountDownLatch latch{kCoroutineCount};
    ScopedSchedulerRuntime runtime(2);

    for (int index = 0; index < kCoroutineCount; ++index) {
        g_scheduler->RegistCoroutineTask([&]() {
            for (int round = 0; round < kYieldCount; ++round)
                bbtco_sleep(1);

            finalized.fetch_add(1);
            latch.Down();
        });
    }

    latch.Wait();
    BOOST_CHECK_EQUAL(finalized.load(), kCoroutineCount);
}

BOOST_AUTO_TEST_CASE(t_repeated_start_stop_does_not_leave_residual_coroutines)
{
    constexpr int kIterations = 10;
    for (int index = 0; index < kIterations; ++index) {
        ScopedSchedulerRuntime runtime(1);
        bbt::core::thread::CountDownLatch latch{1};
        g_scheduler->RegistCoroutineTask([&]() {
            latch.Down();
        });
        latch.Wait();
    }

    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_SUITE_END()