#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
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
    ExceptionHandleCallback callback;
    bool saved{false};
} g_cfg;

std::atomic_bool g_started{false};
}

BOOST_AUTO_TEST_SUITE(ExceptionStopContractTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    auto* cfg = g_bbt_coroutine_config.get();
    if (!g_cfg.saved) {
        g_cfg.threads = cfg->m_cfg_static_thread_num;
        g_cfg.stack_size = cfg->m_cfg_stack_size;
        g_cfg.stack_protect = cfg->m_cfg_stack_protect;
        g_cfg.callback = cfg->m_ext_coevent_exception_callback;
        g_cfg.saved = true;
    }

    cfg->m_cfg_static_thread_num = 1;
    cfg->m_cfg_stack_protect = false;
    cfg->m_ext_coevent_exception_callback = nullptr;

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    g_started.store(true);
    BOOST_REQUIRE(g_scheduler->IsRunning());
}

BOOST_AUTO_TEST_CASE(t_exception_ptr_saved_when_callback)
{
    BOOST_REQUIRE(g_started.load());
    bbt::core::thread::CountDownLatch done{1};
    std::exception_ptr saved;

    auto prev = g_bbt_coroutine_config->m_ext_coevent_exception_callback;
    g_bbt_coroutine_config->m_ext_coevent_exception_callback =
        [&](const bbt::core::errcode::IErrcode&) {
            if (auto co = g_bbt_tls_coroutine_co)
                saved = co->GetException();
            done.Down();
        };

    bbtco []() { throw std::runtime_error("eptr-panic"); };
    done.Wait();
    g_bbt_coroutine_config->m_ext_coevent_exception_callback = prev;

    BOOST_REQUIRE(saved != nullptr);
    try {
        std::rethrow_exception(saved);
        BOOST_FAIL("expected rethrow");
    } catch (const std::runtime_error& e) {
        BOOST_CHECK_EQUAL(std::string(e.what()), "eptr-panic");
    }
}

BOOST_AUTO_TEST_CASE(t_unhandled_exception_counted_without_callback)
{
    BOOST_REQUIRE(g_started.load());
    const auto before = g_bbt_coroutine_config->m_unhandled_exception_count.load();

    g_bbt_coroutine_config->m_ext_coevent_exception_callback = nullptr;
    bbtco []() { throw std::runtime_error("count-panic"); };

    /* 无回调只能轮询计数；上限 1s */
    for (int i = 0; i < 100; ++i) {
        if (g_bbt_coroutine_config->m_unhandled_exception_count.load() > before)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_CHECK_GT(
        g_bbt_coroutine_config->m_unhandled_exception_count.load(),
        before);
}

BOOST_AUTO_TEST_CASE(t_exception_callback_throw_isolated)
{
    BOOST_REQUIRE(g_started.load());
    bbt::core::thread::CountDownLatch boom{1};
    bbt::core::thread::CountDownLatch alive{1};

    auto prev = g_bbt_coroutine_config->m_ext_coevent_exception_callback;
    g_bbt_coroutine_config->m_ext_coevent_exception_callback =
        [&](const bbt::core::errcode::IErrcode&) {
            boom.Down();
            throw std::runtime_error("callback boom");
        };

    bbtco []() { throw std::runtime_error("task boom"); };
    boom.Wait();

    g_bbt_coroutine_config->m_ext_coevent_exception_callback = prev;
    bbtco [&]() { alive.Down(); };
    alive.Wait();
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(t_timeout_is_status_not_exception)
{
    BOOST_REQUIRE(g_started.load());
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<int> rc{-999};
    std::atomic_bool threw{false};

    bbtco [&]() {
        try {
            rc.store(g_bbt_tls_coroutine_co->YieldUntilTimeout(20));
        } catch (...) {
            threw.store(true);
        }
        done.Down();
    };

    done.Wait();
    BOOST_CHECK(!threw.load());
    BOOST_CHECK_NE(rc.load(), -999);
}

BOOST_AUTO_TEST_CASE(t_stop_does_not_drain_sleep)
{
    BOOST_REQUIRE(g_started.load());
    std::atomic_bool finished{false};
    bbtco [&]() {
        g_bbt_tls_coroutine_co->YieldUntilTimeout(5000);
        finished.store(true);
    };

    const auto begin = bbt::core::clock::gettime_mono();
    g_scheduler->Stop();
    g_started.store(false);
    const auto elapsed = bbt::core::clock::gettime_mono() - begin;

    BOOST_CHECK_LT(elapsed, 2000);
    BOOST_CHECK(!finished.load());
}

BOOST_AUTO_TEST_CASE(t_end)
{
    if (g_started.exchange(false))
        g_scheduler->Stop();

    if (g_cfg.saved) {
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = g_cfg.threads;
        cfg->m_cfg_stack_size = g_cfg.stack_size;
        cfg->m_cfg_stack_protect = g_cfg.stack_protect;
        cfg->m_ext_coevent_exception_callback = g_cfg.callback;
        g_cfg.saved = false;
    }
}

BOOST_AUTO_TEST_SUITE_END()
