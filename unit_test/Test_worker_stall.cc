#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

// #277 Worker 无进展检测：死循环/外部阻塞协程占住 worker 超过阈值时，
// 调度线程（独立于 worker）通过锁存快照上报现场（co id/desc/运行时长/积压）。
// 同一停顿（同一 begin_ts）只报一次；正常挂起（sleep/parked）不得误报。

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(WorkerStallTest)

namespace
{
std::atomic<int> g_stall_reports{0};
WorkerStallInfo g_last_stall{};

/* RAII：断言失败/异常也必须停机+摘回调，否则泄漏的 spin 协程污染后续用例 */
struct ProbeFixture
{
    ProbeFixture()
    {
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = 2;   // 一个 worker 可被死循环占住，另一个保活
        cfg->m_cfg_stack_size = 8192;
        cfg->m_cfg_stack_protect = false;
        cfg->m_cfg_worker_stall_warn_ms = 200;
        g_stall_reports = 0;
        g_last_stall = WorkerStallInfo{};
        cfg->m_ext_worker_stall_callback = [](const WorkerStallInfo& info) {
            g_last_stall = info;
            ++g_stall_reports;
        };
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    }
    ~ProbeFixture()
    {
        g_scheduler->Stop();
        g_bbt_coroutine_config->m_ext_worker_stall_callback = nullptr;
    }
};
}

// 死循环协程占住 worker：调度线程必须检出并上报现场
BOOST_AUTO_TEST_CASE(t_spin_worker_is_reported)
{
    ProbeFixture probe;
    std::atomic_bool release_flag{false};
    // 结束时先释放 spin，fixture 的 Stop 才能 join worker
    struct Releaser { std::atomic_bool& f; ~Releaser() { f = true; } } releaser{release_flag};

    bbtco_desc("spin-stall-probe") [&]() {
        while (!release_flag.load(std::memory_order_acquire))
            ;   // 无让出点，占死 worker
    };

    // 阈值 200ms，给足时间至少触发一次
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    BOOST_CHECK(g_stall_reports.load() >= 1);
    BOOST_CHECK_EQUAL(g_last_stall.m_desc, "spin-stall-probe");
    BOOST_CHECK(g_last_stall.m_co_id > 0);
    BOOST_CHECK(g_last_stall.m_running_us >= 200'000);
}

// 同一停顿不重复上报：让 spin 持续 ~1s（阈值 200ms），期望恰好 1 次
BOOST_AUTO_TEST_CASE(t_same_stall_reported_once)
{
    ProbeFixture probe;
    std::atomic_bool release_flag{false};
    struct Releaser { std::atomic_bool& f; ~Releaser() { f = true; } } releaser{release_flag};

    bbtco [&]() {
        while (!release_flag.load(std::memory_order_acquire))
            ;
    };

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    BOOST_CHECK_EQUAL(g_stall_reports.load(), 1);
}

// 正常挂起不误报：parked 协程不占 worker，快照不得命中
BOOST_AUTO_TEST_CASE(t_parked_coroutine_not_reported)
{
    ProbeFixture probe;
    bbt::core::thread::CountDownLatch done{1};
    bbtco [&]() {
        bbtco_sleep(300);   // 挂起 300ms > 阈值 200ms，但不得算停顿
        done.Down();
    };

    done.Wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BOOST_CHECK_EQUAL(g_stall_reports.load(), 0);
}

BOOST_AUTO_TEST_CASE(t_end)
{
}

BOOST_AUTO_TEST_SUITE_END()
