#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <mutex>

#include <atomic>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
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
}

BOOST_AUTO_TEST_SUITE(SchedulerApiTest)

BOOST_AUTO_TEST_CASE(t_singleton_is_g_scheduler)
{
    BOOST_CHECK_EQUAL(g_scheduler.get(), Scheduler::GetInstance().get());
}

/* Start 一次性：初始化完成后重复 Start 抛 std::logic_error，不重置配置/队列、
 * 不新增 worker。驱动模式不能在同一可执行内切换，LoopOnce 用例在
 * Test_scheduler_noloop 单独成目标。 */
BOOST_AUTO_TEST_CASE(t_start_once_then_reject_repeat)
{
    auto* cfg = g_bbt_coroutine_config.get();
    cfg->m_cfg_static_thread_num = 1;
    cfg->m_cfg_stack_protect = false;

    EnsureRuntime();
    BOOST_REQUIRE(g_scheduler->IsInitialized());

    BOOST_CHECK_THROW(g_scheduler->Start(), std::logic_error);
    BOOST_CHECK_THROW(g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD), std::logic_error);
    BOOST_CHECK(g_scheduler->IsInitialized());

    bbt::core::thread::CountDownLatch done{1};
    g_scheduler->RegistCoroutineTask([&]() { done.Down(); });
    BOOST_CHECK_EQUAL(done.WaitTimeout(3000), 0);
}

BOOST_AUTO_TEST_CASE(t_regist_succ_out_param)
{
    EnsureRuntime();
    bbt::core::thread::CountDownLatch done{1};
    bool succ = false;
    g_scheduler->RegistCoroutineTask([&]() { done.Down(); }, succ);
    BOOST_CHECK(succ);
    BOOST_CHECK_EQUAL(done.WaitTimeout(3000), 0);
}

BOOST_AUTO_TEST_SUITE_END()
