#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <mutex>
#include <set>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
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

BOOST_AUTO_TEST_SUITE(SchedulerExecModelTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    auto* cfg = g_bbt_coroutine_config.get();
    if (!g_cfg.saved) {
        g_cfg.threads = cfg->m_cfg_static_thread_num;
        g_cfg.stack_size = cfg->m_cfg_stack_size;
        g_cfg.stack_protect = cfg->m_cfg_stack_protect;
        g_cfg.saved = true;
    }

    cfg->m_cfg_static_thread_num = 2;
    cfg->m_cfg_stack_size = 4096;
    cfg->m_cfg_stack_protect = false;

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    g_started.store(true);
    BOOST_REQUIRE(g_scheduler->IsRunning());
}

BOOST_AUTO_TEST_CASE(t_tls_outside_coroutine_is_zero)
{
    BOOST_REQUIRE(g_started.load());
    BOOST_CHECK_EQUAL(GetLocalCoroutineId(), 0u);
}

BOOST_AUTO_TEST_CASE(t_tls_inside_coroutine)
{
    BOOST_REQUIRE(g_started.load());
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<CoroutineId> co_id{0};
    std::atomic<ProcesserId> proc_id{0};

    bbtco [&]() {
        co_id.store(GetLocalCoroutineId());
        auto proc = Processer::GetLocalProcesser();
        if (proc)
            proc_id.store(proc->GetId());
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_NE(co_id.load(), 0u);
    BOOST_CHECK_NE(proc_id.load(), 0u);
}

BOOST_AUTO_TEST_CASE(t_two_workers_run_two_coroutines)
{
    BOOST_REQUIRE(g_started.load());
    bbt::core::thread::CountDownLatch entered{2};
    bbt::core::thread::CountDownLatch release{1};
    bbt::core::thread::CountDownLatch done{2};
    ProcesserId ids[2]{0, 0};
    std::atomic<int> slot{0};

    auto body = [&]() {
        const int i = slot.fetch_add(1);
        ids[i] = Processer::GetLocalProcesser()->GetId();
        BOOST_CHECK_NE(GetLocalCoroutineId(), 0u);
        entered.Down();
        release.Wait();
        done.Down();
    };

    bbtco body;
    bbtco body;

    entered.Wait();
    BOOST_CHECK_NE(ids[0], 0u);
    BOOST_CHECK_NE(ids[1], 0u);
    BOOST_CHECK_NE(ids[0], ids[1]);
    release.Down();
    done.Wait();
}

BOOST_AUTO_TEST_CASE(t_yield_keeps_single_running_and_valid_tls)
{
    BOOST_REQUIRE(g_started.load());
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<int> overlap{0};
    std::atomic<int> overlap_max{0};
    std::set<ProcesserId> procs;
    std::mutex procs_mu;

    bbtco [&]() {
        for (int i = 0; i < 8; ++i) {
            const int now = overlap.fetch_add(1) + 1;
            int seen = overlap_max.load();
            while (now > seen && !overlap_max.compare_exchange_weak(seen, now)) {}
            {
                std::lock_guard<std::mutex> lk(procs_mu);
                procs.insert(Processer::GetLocalProcesser()->GetId());
            }
            BOOST_CHECK_EQUAL(g_bbt_tls_coroutine_co->GetStatus(), CoroutineStatus::CO_RUNNING);
            overlap.fetch_sub(1);
            bbtco_yield;
        }
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(overlap_max.load(), 1);
    BOOST_REQUIRE(!procs.empty());
    for (auto id : procs)
        BOOST_CHECK_NE(id, 0u);
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
