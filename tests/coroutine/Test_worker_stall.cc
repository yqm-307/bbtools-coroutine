#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <boost/test/unit_test.hpp>

// Worker 无进展检测：死循环/外部阻塞协程占住 worker 超过阈值时，
// 调度线程（独立于 worker）通过锁存快照上报现场（co id/desc/运行时长/积压）。
// 同一停顿（同一 begin_ts）只报一次；正常挂起（sleep/parked）不得误报。
// 快照改由 Processer::m_exec_mutex 保护（写者=worker，读者=调度线程），
// 一致性由互斥 happens-before 给出；本文件同时修复自身并发与寿命缺陷：
//   * 配置/回调只在唯一 Start 前设置，运行期不再写非线程安全配置字段；
//   * 上报经互斥锁进入 g_reports，测试线程读快照副本；
//   * spin 协程退出由 CountDownLatch 因果等待，栈上引用不先于协程完成而销毁；
//   * (co_id, desc) 配对用协程内自报的真值映射校验，不是"desc∈{A,B}+id>0"。
// 开关边界用进程参数区分：默认 warn_ms=200（启用），`-- --stall-warn-ms=0`
// 进入关闭模式（独立进程，配置仍只在 Start 前写一次）。

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

namespace
{

/* 进程级一次性配置：warn_ms 由 `-- --stall-warn-ms=N` 参数给出（默认 200）。
 * m_cfg_worker_stall_warn_ms / m_ext_worker_stall_callback / worker 数等
 * 全部为非线程安全字段，只能在 Scheduler::Start 前写一次。 */
size_t StallWarnMsArg()
{
    static const size_t warn_ms = []() {
        auto& ts = boost::unit_test::framework::master_test_suite();
        for (int i = 1; i < ts.argc; ++i)
        {
            const char* arg = ts.argv[i];
            const char* prefix = "--stall-warn-ms=";
            if (strncmp(arg, prefix, strlen(prefix)) == 0)
                return static_cast<size_t>(atoll(arg + strlen(prefix)));
        }
        return size_t{200};
    }();
    return warn_ms;
}

/* 回调接收的完整上报，持锁读副本。回调在调度线程上执行，
 * 只 push 不阻塞（契约：不抛出、不阻塞）。 */
std::mutex                          g_reports_mutex;
std::vector<WorkerStallInfo>        g_reports;

void RecordStall(const WorkerStallInfo& info)
{
    std::lock_guard<std::mutex> lock(g_reports_mutex);
    g_reports.push_back(info);
}

std::vector<WorkerStallInfo> SnapshotReports()
{
    std::lock_guard<std::mutex> lock(g_reports_mutex);
    return g_reports;
}

void ClearReports()
{
    std::lock_guard<std::mutex> lock(g_reports_mutex);
    g_reports.clear();
}

/* 协程内自报的 desc->co_id 真值映射：协程启动即登记自己，早于任何
 * 可能的上报（上报要求该协程已占住 worker 超阈值）。 */
std::mutex                          g_truth_mutex;
std::unordered_map<std::string, CoroutineId> g_desc_to_id;

void RegisterTruth(const char* desc)
{
    std::lock_guard<std::mutex> lock(g_truth_mutex);
    g_desc_to_id.emplace(desc, GetLocalCoroutineId());
}

std::unordered_map<std::string, CoroutineId> SnapshotTruth()
{
    std::lock_guard<std::mutex> lock(g_truth_mutex);
    return g_desc_to_id;
}

void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, [](){
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = 2;   // 一个 worker 可被死循环占住，另一个保活
        cfg->m_cfg_stack_size = 8192;
        cfg->m_cfg_stack_protect = false;
        cfg->m_ext_worker_stall_callback = &RecordStall;
        cfg->m_cfg_worker_stall_warn_ms = StallWarnMsArg();
        Scheduler::GetInstance()->Start();
    });
}

/* 死循环 spin 协程：release 置位后退出循环，随后写 finished 作为完成收据。
 * 测试侧必须先 release 再等 finished，栈上引用（release/finished/entered）
 * 的寿命由完成收据因果保证，不靠析构仅置 flag。
 * finished 是自有原子完成事实，不借用 CountDownLatch::WaitTimeout——
 * 其内部 pthread_cond_timedwait 返回 0 后不复查 m_count，spurious wakeup
 * 下会假报完成，不能当 Join 收据。 */
struct Spinner
{
    std::atomic_bool            release{false};
    std::atomic_bool            finished{false};  // 协程退出前最后一次写本对象

    ~Spinner()
    {
        Stop();
        // 断言提前退出也不允许 worker 继续借用已销毁的栈变量。
        if (!Join(5000))
            std::abort();
    }

    void Stop()
    {
        release.store(true, std::memory_order_release);
    }
    /* 完成收据 = finished 原子真值；deadline 只兜底防挂死，轮询间隔不
     * 作时序判断。 */
    bool Join(int timeout_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!finished.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }
};

/* 等待上报数达到 n（因果等待条件，非固定 sleep）；超时返回 false。 */
bool WaitReportsAtLeast(size_t n, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (SnapshotReports().size() >= n)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return SnapshotReports().size() >= n;
}

/* 启用模式判定延迟到用例体内求值：master_test_suite() 的参数在
 * main()/Boost.Test 初始化后才可用，不能放静态初始化期读取。 */
bool StallEnabled() { return StallWarnMsArg() > 0; }

} // namespace

BOOST_AUTO_TEST_SUITE(WorkerStallTest)

// 死循环协程占住 worker：调度线程必须检出并上报现场
BOOST_AUTO_TEST_CASE(t_spin_worker_is_reported)
{
    if (!StallEnabled()) { BOOST_TEST_MESSAGE("skip: disabled mode"); return; }
    EnsureRuntime();
    ClearReports();
    Spinner spinner;

    bbtco_desc("spin-stall-probe") [&]() {
        RegisterTruth("spin-stall-probe");
        while (!spinner.release.load(std::memory_order_acquire))
            ;   // 无让出点，占死 worker
        spinner.finished.store(true, std::memory_order_release);
    };

    // 阈值 200ms：因果等待首次上报（上限 5s 兜底），不靠固定时长 sleep
    BOOST_REQUIRE(WaitReportsAtLeast(1, 5000));
    spinner.Stop();
    BOOST_REQUIRE(spinner.Join(5000));

    auto reports = SnapshotReports();
    BOOST_REQUIRE_GE(reports.size(), 1u);
    const auto& info = reports.front();
    BOOST_CHECK_EQUAL(info.m_desc, "spin-stall-probe");
    auto truth = SnapshotTruth();
    auto it = truth.find(info.m_desc);
    BOOST_REQUIRE(it != truth.end());
    BOOST_CHECK_EQUAL(info.m_co_id, it->second);
    BOOST_CHECK(info.m_co_id > 0);
    BOOST_CHECK(info.m_running_us >= 200'000);
}

// 同一停顿不重复上报：让 spin 持续覆盖多个扫描拍，期望恰好 1 次
BOOST_AUTO_TEST_CASE(t_same_stall_reported_once)
{
    if (!StallEnabled()) { BOOST_TEST_MESSAGE("skip: disabled mode"); return; }
    EnsureRuntime();
    ClearReports();
    Spinner spinner;

    bbtco_desc("stall-dedup-probe") [&]() {
        RegisterTruth("stall-dedup-probe");
        while (!spinner.release.load(std::memory_order_acquire))
            ;
        spinner.finished.store(true, std::memory_order_release);
    };

    BOOST_REQUIRE(WaitReportsAtLeast(1, 5000));
    // 首次上报后再覆盖若干扫描拍（~1s），去重不得产生第二次上报
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    spinner.Stop();
    BOOST_REQUIRE(spinner.Join(5000));

    auto reports = SnapshotReports();
    size_t hits = 0;
    for (const auto& r : reports)
        if (r.m_desc == "stall-dedup-probe")
            ++hits;
    BOOST_CHECK_EQUAL(hits, 1u);
}

// 并发快照一致性：两个 spin 协程同时占住 worker（latch 保证并发窗口），
// 每条上报的 (co_id, desc) 配对必须与协程自报真值一致——撕裂快照会
// 产生 truth 中不存在的配对（如 idA+descB），直接判负。
BOOST_AUTO_TEST_CASE(t_concurrent_snapshot_reports_coherent_payload)
{
    if (!StallEnabled()) { BOOST_TEST_MESSAGE("skip: disabled mode"); return; }
    EnsureRuntime();
    ClearReports();
    constexpr int kSpinners = 2;
    /* entered 必须先于 Spinner 声明：局部对象逆序析构，断言失败展开时
     * Spinner 先 Stop+Join（等协程退出），协程在此之前仍会 entered.Down()
     * ——entered 必须活过两个 Spinner 退出，不能先析构。 */
    bbt::core::thread::CountDownLatch entered{kSpinners};
    Spinner spinner_a, spinner_b;

    bbtco_desc("stall-probe-A") [&]() {
        RegisterTruth("stall-probe-A");
        entered.Down();
        while (!spinner_a.release.load(std::memory_order_acquire))
            ;
        spinner_a.finished.store(true, std::memory_order_release);
    };
    bbtco_desc("stall-probe-B") [&]() {
        RegisterTruth("stall-probe-B");
        entered.Down();
        while (!spinner_b.release.load(std::memory_order_acquire))
            ;
        spinner_b.finished.store(true, std::memory_order_release);
    };

    BOOST_REQUIRE(entered.WaitTimeout(5000) == 0);
    // 因果等待两条上报到位（每个停顿协程各一条）
    BOOST_REQUIRE(WaitReportsAtLeast(2, 5000));
    spinner_a.Stop();
    spinner_b.Stop();
    BOOST_REQUIRE(spinner_a.Join(5000));
    BOOST_REQUIRE(spinner_b.Join(5000));

    auto truth = SnapshotTruth();
    // 真值表为进程累计记录；这里只核对本轮两条上报的映射。
    BOOST_REQUIRE(truth.count("stall-probe-A") == 1);
    BOOST_REQUIRE(truth.count("stall-probe-B") == 1);
    auto reports = SnapshotReports();
    size_t matched = 0;
    for (const auto& r : reports)
    {
        if (r.m_desc != "stall-probe-A" && r.m_desc != "stall-probe-B")
            continue;
        ++matched;
        auto it = truth.find(r.m_desc);
        BOOST_REQUIRE(it != truth.end());
        BOOST_CHECK_EQUAL(r.m_co_id, it->second);   // 配对一致性
        BOOST_CHECK(!r.m_desc.empty());
    }
    BOOST_CHECK_EQUAL(matched, 2u);
}

// 正常挂起不误报：parked 协程不占 worker，快照不得命中
BOOST_AUTO_TEST_CASE(t_parked_coroutine_not_reported)
{
    if (!StallEnabled()) { BOOST_TEST_MESSAGE("skip: disabled mode"); return; }
    EnsureRuntime();
    ClearReports();
    /* done 由协程 lambda 以值持有 shared_ptr：WaitTimeout 断言失败展开时
     * 局部引用先销毁，但 parked 协程对象仍持有 latch 直到自身析构，
     * 晚恢复后的 done->Down() 不会触碰已销毁栈对象。 */
    auto done = std::make_shared<bbt::core::thread::CountDownLatch>(1);

    bbtco [done]() {
        bbtco_sleep(300);   // 挂起 300ms > 阈值 200ms，但不得算停顿
        done->Down();
    };

    BOOST_REQUIRE(done->WaitTimeout(5000) == 0);
    // 再覆盖若干扫描拍确认无延迟误报
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BOOST_CHECK(SnapshotReports().empty());
}

// 关闭边界：warn_ms=0（独立进程参数）时调度线程整段跳过扫描，
// 死循环占住 worker 也不得产生任何上报（默认零诊断采集）。
// 本用例只在 -- --stall-warn-ms=0 模式下有实义；启用模式下自检跳过。
BOOST_AUTO_TEST_CASE(t_disabled_warn_ms_no_reports)
{
    if (StallEnabled()) { BOOST_TEST_MESSAGE("skip: enabled mode"); return; }
    EnsureRuntime();
    ClearReports();
    Spinner spinner;

    bbtco_desc("disabled-stall-probe") [&]() {
        while (!spinner.release.load(std::memory_order_acquire))
            ;
        spinner.finished.store(true, std::memory_order_release);
    };

    // 关闭模式无回调可等：覆盖若干扫描拍后断言零上报（上限兜底在外层 TIMEOUT）
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    spinner.Stop();
    BOOST_REQUIRE(spinner.Join(5000));
    BOOST_CHECK(SnapshotReports().empty());
}

BOOST_AUTO_TEST_CASE(t_end)
{
}

BOOST_AUTO_TEST_SUITE_END()
