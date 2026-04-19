#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>

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

BOOST_AUTO_TEST_SUITE(CoroutineLifecycleTeardownTest)

BOOST_AUTO_TEST_CASE(t_yield_with_callback_preserves_registration_order)
{
    Coroutine::Ptr coroutine = nullptr;
    std::atomic_int step{0};
    std::atomic_int callback_step{0};
    std::atomic_int resume_step{0};

    coroutine = Coroutine::Create(4096, [&]() {
        BOOST_REQUIRE_EQUAL(coroutine->YieldWithCallback([&]() {
            callback_step.store(step.fetch_add(1) + 1);
            return true;
        }), 0);

        resume_step.store(step.fetch_add(1) + 1);
    }, false);

    coroutine->Resume();
    BOOST_CHECK_EQUAL(callback_step.load(), 1);
    BOOST_CHECK_EQUAL(resume_step.load(), 0);
    BOOST_CHECK_EQUAL(coroutine->GetStatus(), CoroutineStatus::CO_SUSPEND);

    coroutine->Resume();
    BOOST_CHECK_EQUAL(resume_step.load(), 2);
    BOOST_CHECK_EQUAL(coroutine->GetStatus(), CoroutineStatus::CO_FINAL);
    delete coroutine;
}

BOOST_AUTO_TEST_CASE(t_exception_path_cleans_active_await_event)
{
    ScopedSchedulerRuntime runtime(1);
    std::atomic_bool exception_seen{false};
    auto previous = g_bbt_coroutine_config->m_ext_coevent_exception_callback;

    g_bbt_coroutine_config->m_ext_coevent_exception_callback = [&](const bbt::core::errcode::IErrcode&) {
        exception_seen.store(true);
    };

    g_scheduler->RegistCoroutineTask([]() {
        auto* coroutine = g_bbt_tls_coroutine_co;
        BOOST_REQUIRE(coroutine != nullptr);
        BOOST_REQUIRE_EQUAL(coroutine->YieldUntilTimeout(10), 0);
        throw std::runtime_error("lifecycle teardown");
    });

    for (int i = 0; i < 200 && !exception_seen.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    g_bbt_coroutine_config->m_ext_coevent_exception_callback = previous;
    BOOST_CHECK(exception_seen.load());
}

BOOST_AUTO_TEST_SUITE_END()