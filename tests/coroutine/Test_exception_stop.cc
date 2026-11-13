#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <exception>

#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

namespace {
/**
 * 回调在 Start 时冻结；CTest 将各场景放在独立进程，保留真正的空回调路径。
 */
void StartRuntime(ExceptionHandleCallback callback = nullptr)
{
    auto* cfg = g_bbt_coroutine_config.get();
    cfg->m_cfg_static_thread_num = 1;
    cfg->m_cfg_stack_protect = false;
    cfg->m_ext_coevent_exception_callback = std::move(callback);
    g_scheduler->Start();
    BOOST_REQUIRE(g_scheduler->IsInitialized());
}
}

BOOST_AUTO_TEST_SUITE(ExceptionContractTest)

BOOST_AUTO_TEST_CASE(t_exception_ptr_saved_when_callback)
{
    bbt::core::thread::CountDownLatch done{1};
    std::exception_ptr saved;

    StartRuntime([&](const bbt::core::errcode::IErrcode&) {
        if (auto co = g_bbt_tls_coroutine_co)
            saved = co->GetException();
        done.Down();
    });

    bbtco[]()
    {
        throw std::runtime_error("eptr-panic");
    };
    done.Wait();

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
    StartRuntime();
    const auto before = g_bbt_coroutine_config->m_unhandled_exception_count.load();

    bbtco[]()
    {
        throw std::runtime_error("count-panic");
    };

    /**
     * 无回调只能轮询计数；上限 1s
     */
    for (int i = 0; i < 100; ++i) {
        if (g_bbt_coroutine_config->m_unhandled_exception_count.load() > before)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_CHECK_GT(g_bbt_coroutine_config->m_unhandled_exception_count.load(), before);
}

BOOST_AUTO_TEST_CASE(t_exception_callback_throw_isolated)
{
    bbt::core::thread::CountDownLatch boom{1};
    bbt::core::thread::CountDownLatch alive{1};

    StartRuntime([&](const bbt::core::errcode::IErrcode&) {
        boom.Down();
        throw std::runtime_error("callback boom");
    });

    bbtco[]()
    {
        throw std::runtime_error("task boom");
    };
    boom.Wait();

    bbtco[&]()
    {
        alive.Down();
    };
    alive.Wait();
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(t_timeout_is_status_not_exception)
{
    StartRuntime();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<int> rc{-999};
    std::atomic_bool threw{false};

    bbtco[&]()
    {
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

BOOST_AUTO_TEST_SUITE_END()
