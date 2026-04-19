#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>

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

BOOST_AUTO_TEST_SUITE(PublicApiContractTest)

BOOST_AUTO_TEST_CASE(t_public_helpers_return_empty_state_outside_coroutine)
{
    BOOST_CHECK_EQUAL(GetLocalCoroutineId(), 0);
    BOOST_CHECK_EQUAL(GetLocalCoroutineStackSize(), 0);
}

BOOST_AUTO_TEST_CASE(t_public_registration_requires_started_scheduler)
{
    bool succ = true;
    bbtco_noexcept(&succ) []() {};
    BOOST_CHECK(!succ);
}

BOOST_AUTO_TEST_CASE(t_public_stop_rejects_worker_context)
{
    ScopedSchedulerRuntime runtime(1);
    std::atomic_bool threw{false};
    bbt::core::thread::CountDownLatch latch{1};

    bbtco [&]() {
        try {
            g_scheduler->Stop();
        }
        catch (const std::runtime_error&) {
            threw.store(true);
        }

        latch.Down();
    };

    latch.Wait();
    BOOST_CHECK(threw.load());
    BOOST_CHECK(g_scheduler->IsRunning());
}

BOOST_AUTO_TEST_SUITE_END()