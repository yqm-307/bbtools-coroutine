#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>

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
    bool saved{false};
} g_cfg;

std::atomic_bool g_started{false};
}

BOOST_AUTO_TEST_SUITE(CoroutineCancelTest)

BOOST_AUTO_TEST_CASE(t_begin)
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
    g_started.store(true);
    BOOST_REQUIRE(g_scheduler->IsRunning());
}

BOOST_AUTO_TEST_CASE(t_cancel_wakes_timeout_wait)
{
    BOOST_REQUIRE(g_started.load());
    bbt::core::thread::CountDownLatch parked{1};
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<Coroutine*> target{nullptr};
    std::atomic<int> elapsed_ms{-1};
    std::atomic<bool> saw_cancel{false};

    bbtco [&]() {
        target.store(g_bbt_tls_coroutine_co);
        parked.Down();
        const auto begin = bbt::core::clock::gettime_mono();
        g_bbt_tls_coroutine_co->YieldUntilTimeout(5000);
        elapsed_ms.store(static_cast<int>(bbt::core::clock::gettime_mono() - begin));
        saw_cancel.store(g_bbt_tls_coroutine_co->IsCancelRequested());
        done.Down();
    };

    parked.Wait();
    target.load()->RequestCancel();
    done.Wait();

    BOOST_CHECK_LT(elapsed_ms.load(), 1000);
    BOOST_CHECK(saw_cancel.load());
}

BOOST_AUTO_TEST_CASE(t_cancel_before_wait_returns_immediately)
{
    BOOST_REQUIRE(g_started.load());
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<int> elapsed_ms{-1};

    bbtco [&]() {
        g_bbt_tls_coroutine_co->RequestCancel();
        const auto begin = bbt::core::clock::gettime_mono();
        g_bbt_tls_coroutine_co->YieldUntilTimeout(5000);
        elapsed_ms.store(static_cast<int>(bbt::core::clock::gettime_mono() - begin));
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_LT(elapsed_ms.load(), 1000);
}

BOOST_AUTO_TEST_CASE(t_cancel_running_does_not_kill)
{
    BOOST_REQUIRE(g_started.load());
    bbt::core::thread::CountDownLatch started{1};
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<Coroutine*> target{nullptr};
    std::atomic<int> steps{0};

    bbtco [&]() {
        target.store(g_bbt_tls_coroutine_co);
        started.Down();
        /* do-while：先执行一轮再检查取消——消除"cancel 在首次检查前到达、
         * steps 恒 0"的时序竞态（CI 实测），且仍验证协作式不强杀 */
        do {
            steps.fetch_add(1);
            bbtco_yield;
        } while (!g_bbt_tls_coroutine_co->IsCancelRequested());
        done.Down();
    };

    started.Wait();
    target.load()->RequestCancel();
    done.Wait();
    BOOST_CHECK_GE(steps.load(), 1);
}

BOOST_AUTO_TEST_CASE(t_cancel_runs_raii_dtors)
{
    BOOST_REQUIRE(g_started.load());
    bbt::core::thread::CountDownLatch parked{1};
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<Coroutine*> target{nullptr};
    std::atomic_int dtors{0};

    struct Guard {
        std::atomic_int* n;
        ~Guard() { n->fetch_add(1); }
    };

    bbtco [&]() {
        /* RAII 析断必须在 done.Down() 之前完成——否则主线程被唤醒后
         * 可能在析构前读 dtors（CI 实测竞态） */
        {
            Guard g{&dtors};
            target.store(g_bbt_tls_coroutine_co);
            parked.Down();
            g_bbt_tls_coroutine_co->YieldUntilTimeout(5000);
        }
        done.Down();
    };

    parked.Wait();
    target.load()->RequestCancel();
    done.Wait();
    BOOST_CHECK_EQUAL(dtors.load(), 1);
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
        g_cfg.saved = false;
    }
}

BOOST_AUTO_TEST_SUITE_END()
