#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <mutex>

// io::GetExecutor 公共桥接契约：
// ① post 到 GetExecutor() 的 handler 由 Scheduler 现有 PollOnce 驱动并完成
// ② 重复 GetExecutor() 绑定同一 io_context，无独立事件循环
// ③ GetExecutor()/post 均不新增线程

#include <atomic>
#include <chrono>
#include <thread>

// 线程数动态证据依赖 Linux /proc/self/task；非 Linux 平台不引用
// filesystem/directory_iterator，仅保留 executor identity 与
// 运行期 post/身份稳定的通用断言。
#if defined(__linux__)
#include <filesystem>
#endif

#include <boost/asio.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/io/IoExecutor.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

/* 进程寿命模型：runtime 只初始化一次，重复 Start 抛 std::logic_error。 */
namespace
{
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, [](){
        bbt::coroutine::detail::Scheduler::GetInstance()->Start();
    });
}
}

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
    }
    ~CoFixture()
    {
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

// ② 线程调度模式：post 的 handler 由调度线程的 PollOnce 循环真实推进。
BOOST_AUTO_TEST_CASE(t_post_handler_runs_under_running_scheduler)
{
    CoFixture fx;
    EnsureRuntime();

    std::atomic_int n{0};
    boost::asio::post(io::GetExecutor(), [&]() { n.fetch_add(1); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (n.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    BOOST_CHECK_EQUAL(n.load(), 1);
}

// ③ 重复获取绑定同一事件循环且不新增线程；运行期身份稳定。
// （NO_LOOP/LoopOnce 手动驱动路径见 Test_scheduler_noloop 专用目标）
BOOST_AUTO_TEST_CASE(t_executor_identity_stable_no_new_thread)
{
    CoFixture fx;
    EnsureRuntime();

    const auto ex1 = io::GetExecutor();
    const auto ex2 = io::GetExecutor();
    BOOST_CHECK(ex1 == ex2);    // 同一 io_context，无独立事件循环

#if defined(__linux__)
    const size_t before = CountSelfThreads();
    BOOST_REQUIRE(before > 0);
#endif

    std::atomic_int done{0};
    for (int i = 0; i < 64; ++i)
        boost::asio::post(io::GetExecutor(), [&]() { done.fetch_add(1); });

#if defined(__linux__)
    BOOST_CHECK_EQUAL(CountSelfThreads(), before);   // post 不新增线程
#endif

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (done.load() < 64 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // 通用断言：64 个 handler 由运行时的 PollOnce 驱动排空，无独立事件循环
    BOOST_CHECK_EQUAL(done.load(), 64);
    BOOST_CHECK(io::GetExecutor() == ex1);      // 身份稳定

#if defined(__linux__)
    BOOST_CHECK_EQUAL(CountSelfThreads(), before);  // 运行期线程数不变
#endif
}

BOOST_AUTO_TEST_SUITE_END()
