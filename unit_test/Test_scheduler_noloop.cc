/**
 * @file Test_scheduler_noloop.cc
 * @brief NO_LOOP 驱动模式 + LoopOnce 专用目标。
 *
 * 驱动模式不能与 THREAD 模式共存于同一可执行（Start 一次性、重复 Start 抛
 * std::logic_error），因此单列一个目标：覆盖 NO_LOOP 下事件循环只能由
 * LoopOnce→PollOnce 手动驱动、初始化查询与重复 Start 拒绝，以及
 * post 不新增线程。原 Test_io_executor.cc ① 与 Test_scheduler_api.cc 的
 * NO_LOOP 用例迁入此目标。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#if defined(__linux__)
#include <filesystem>
#endif

#include <boost/asio.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/io/IoExecutor.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;

namespace
{

#if defined(__linux__)
size_t CountSelfThreads()
{
    size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
        (void)entry;
        ++n;
    }
    return n;
}
#endif

void StartNoLoopOnce()
{
    static std::once_flag once;
    std::call_once(once, [](){
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = 1;
        cfg->m_cfg_stack_protect = false;
        bbt::coroutine::detail::Scheduler::GetInstance()->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    });
}

} // namespace

BOOST_AUTO_TEST_SUITE(SchedulerNoLoopTest)

/* NO_LOOP：未驱动不得推进；LoopOnce 在调用线程上完成，证明无隐藏驱动 */
BOOST_AUTO_TEST_CASE(t_post_handler_driven_by_looponce)
{
    StartNoLoopOnce();
    BOOST_REQUIRE(g_scheduler->IsInitialized());

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

/* 重复 Start 拒绝：不重置配置/队列、不新增 worker；LoopOnce 仍可用 */
BOOST_AUTO_TEST_CASE(t_repeat_start_rejected_keep_driving)
{
    StartNoLoopOnce();

    BOOST_CHECK_THROW(g_scheduler->Start(), std::logic_error);
    BOOST_CHECK_THROW(g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP), std::logic_error);
    BOOST_CHECK(g_scheduler->IsInitialized());

    std::atomic_int n{0};
    boost::asio::post(io::GetExecutor(), [&]() { n.fetch_add(1); });
    g_scheduler->LoopOnce();
    BOOST_CHECK_EQUAL(n.load(), 1);
}

/* post 不新增线程（Linux /proc/self/task 计数） */
BOOST_AUTO_TEST_CASE(t_post_does_not_spawn_thread)
{
    StartNoLoopOnce();

    const auto ex1 = io::GetExecutor();
    BOOST_CHECK(ex1 == io::GetExecutor());   // 同一 io_context，无独立事件循环

#if defined(__linux__)
    const size_t before = CountSelfThreads();
    BOOST_REQUIRE(before > 0);
#endif

    std::atomic_int done{0};
    for (int i = 0; i < 64; ++i)
        boost::asio::post(io::GetExecutor(), [&]() { done.fetch_add(1); });

#if defined(__linux__)
    BOOST_CHECK_EQUAL(CountSelfThreads(), before);
#endif

    g_scheduler->LoopOnce();
    BOOST_CHECK_EQUAL(done.load(), 64);
    BOOST_CHECK(io::GetExecutor() == ex1);

#if defined(__linux__)
    BOOST_CHECK_EQUAL(CountSelfThreads(), before);
#endif
}

BOOST_AUTO_TEST_SUITE_END()
