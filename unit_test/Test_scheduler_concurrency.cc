#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <thread>
#include <vector>

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

BOOST_AUTO_TEST_SUITE(SchedulerConcurrencyTest)

BOOST_AUTO_TEST_CASE(t_concurrent_submission_executes_exactly_once)
{
    ScopedSchedulerRuntime runtime(4);
    constexpr int kThreads = 8;
    constexpr int kTasksPerThread = 32;
    std::atomic_int executed{0};
    bbt::core::thread::CountDownLatch latch{kThreads * kTasksPerThread};
    std::vector<std::thread> submitters;

    for (int index = 0; index < kThreads; ++index) {
        submitters.emplace_back([&]() {
            for (int task = 0; task < kTasksPerThread; ++task) {
                g_scheduler->RegistCoroutineTask([&]() {
                    executed.fetch_add(1);
                    latch.Down();
                });
            }
        });
    }

    for (auto& submitter : submitters)
        submitter.join();

    latch.Wait();
    BOOST_CHECK_EQUAL(executed.load(), kThreads * kTasksPerThread);
}

BOOST_AUTO_TEST_CASE(t_scheduler_stop_blocks_new_runnable_expansion)
{
    ScopedSchedulerRuntime runtime(2);

    g_scheduler->Stop();
    BOOST_CHECK_THROW(g_scheduler->RegistCoroutineTask([]() {}), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()