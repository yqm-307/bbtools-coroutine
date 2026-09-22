#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

// io::GetExecutor 公共桥接契约：
// ① post 到 GetExecutor() 的 handler 由 Scheduler 现有 PollOnce 驱动并完成
// ② 重复 GetExecutor() 绑定同一 io_context，无独立事件循环
// ③ GetExecutor()/post/Stop 均不新增线程

#include <atomic>
#include <chrono>
#include <thread>

// 线程数动态证据依赖 Linux /proc/self/task；非 Linux 平台不引用
// filesystem/directory_iterator，仅保留 executor identity 与
// 手动 LoopOnce 排空无隐藏驱动的通用断言。
#if defined(__linux__)
#include <filesystem>
#endif

#include <boost/asio.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/io/IoExecutor.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE(IoExecutorTest)

namespace
{

struct CoFixture
{
    CoFixture()
    {
        auto* cfg = g_bbt_coroutine_config.get();
        m_threads = cfg->m_cfg_static_thread_num;
        m_protect = cfg->m_cfg_stack_protect;
        cfg->m_cfg_static_thread_num = 1;
        cfg->m_cfg_stack_protect = false;
        // 用例自包含：先确保停机态再 Start，避免上个用例遗留 running 态导致双 Start
        if (g_scheduler->IsRunning())
            g_scheduler->Stop();
    }
    ~CoFixture()
    {
        if (g_scheduler->IsRunning())
            g_scheduler->Stop();
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = m_threads;
        cfg->m_cfg_stack_protect = m_protect;
    }
    size_t m_threads;
    bool m_protect;
};

#if defined(__linux__)
size_t CountSelfThreads()
{
    size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task"))
    {
        (void)entry;
        ++n;
    }
    return n;
}
#endif

} // namespace

// ① NO_LOOP 模式下事件循环只能由 LoopOnce→PollOnce 驱动：
// post 后未驱动不得执行；LoopOnce 后在调用线程上完成，证明无隐藏线程。
BOOST_AUTO_TEST_CASE(t_post_handler_driven_by_pollonce)
{
    CoFixture fx;
    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);

    std::atomic_int n{0};
    std::thread::id handler_tid;
    const auto caller_tid = std::this_thread::get_id();

    boost::asio::post(io::GetExecutor(), [&]() {
        handler_tid = std::this_thread::get_id();
        n.fetch_add(1);
    });

    BOOST_CHECK_EQUAL(n.load(), 0);
    g_scheduler->LoopOnce();
    BOOST_CHECK_EQUAL(n.load(), 1);
    BOOST_CHECK(handler_tid == caller_tid);
}

// ② 线程调度模式：post 的 handler 由调度线程的 PollOnce 循环真实推进。
BOOST_AUTO_TEST_CASE(t_post_handler_runs_under_running_scheduler)
{
    CoFixture fx;
    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);

    std::atomic_int n{0};
    boost::asio::post(io::GetExecutor(), [&]() { n.fetch_add(1); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (n.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    BOOST_CHECK_EQUAL(n.load(), 1);
}

// ③ 重复获取绑定同一事件循环且不新增线程；Stop 后 executor 身份不变。
BOOST_AUTO_TEST_CASE(t_executor_identity_stable_no_new_thread)
{
    CoFixture fx;

    const auto ex1 = io::GetExecutor();
    const auto ex2 = io::GetExecutor();
    BOOST_CHECK(ex1 == ex2);    // 同一 io_context，无独立事件循环

#if defined(__linux__)
    const size_t before = CountSelfThreads();
    BOOST_REQUIRE(before > 0);
#endif

    std::atomic_int done{0};
    std::thread::id handler_tid;
    for (int i = 0; i < 64; ++i)
        boost::asio::post(io::GetExecutor(), [&]() {
            handler_tid = std::this_thread::get_id();
            done.fetch_add(1);
        });

#if defined(__linux__)
    BOOST_CHECK_EQUAL(CountSelfThreads(), before);
#endif

    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    g_scheduler->LoopOnce();    // 排空积压 handler
    g_scheduler->Stop();

    // 通用断言：64 个 handler 全部由本次 LoopOnce 在调用线程上排空，无隐藏驱动
    BOOST_CHECK_EQUAL(done.load(), 64);
    BOOST_CHECK(handler_tid == std::this_thread::get_id());
    BOOST_CHECK(io::GetExecutor() == ex1);      // 停机后仍指向同一事件循环

#if defined(__linux__)
    BOOST_CHECK_EQUAL(CountSelfThreads(), before);  // Start/Stop 后线程数回到基线
#endif
}

BOOST_AUTO_TEST_SUITE_END()
