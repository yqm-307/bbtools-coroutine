#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <mutex>

#include <atomic>
#include <limits>
#include <stdexcept>
#include <unistd.h>     // getpagesize()：显式大小按系统页对齐

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

/* S14：显式大小在前的两种注册重载真实可用（不再被忽略），且不改写全局默认；
 * 描述宏与成功输出宏保持原语法并真实运转。旧调用编译/运转由本文件与
 * Test_multi_size_stack_runtime 共同覆盖。 */
BOOST_AUTO_TEST_CASE(t_explicit_stack_size_registration)
{
    EnsureRuntime();

    const size_t page = static_cast<size_t>(getpagesize());
    const size_t request = 64 * 1024 + 1;
    const size_t expected = request + ((page - request % page) % page);

    std::atomic<size_t> seen{0};
    bbt::core::thread::CountDownLatch latch{3};

    g_scheduler->RegistCoroutineTask(request, [&]() {
        seen.store(GetLocalCoroutineStackSize());
        latch.Down();
    }, "explicit-size");

    bool succ = false;
    g_scheduler->RegistCoroutineTask(request, [&]() { latch.Down(); }, succ);
    BOOST_CHECK(succ);

    bbtco_desc("api-desc") [&]() { latch.Down(); };

    BOOST_CHECK_EQUAL(latch.WaitTimeout(5000), 0);
    BOOST_CHECK_EQUAL(seen.load(), expected);
    /* 默认值未被显式调用改写：出厂 12 KiB 仍是不传大小时的唯一来源 */
    BOOST_CHECK_EQUAL(g_bbt_coroutine_config->m_cfg_stack_size, 1024u * 12);
}

/* S14/S9：非法值经注册入口在入队前被拒绝，异常类别符合契约；成功输出重载不抛置 false。 */
BOOST_AUTO_TEST_CASE(t_invalid_stack_size_rejected_via_registration)
{
    EnsureRuntime();

    BOOST_CHECK_THROW(g_scheduler->RegistCoroutineTask(size_t(0), [](){}),
                      std::invalid_argument);
    BOOST_CHECK_THROW(g_scheduler->RegistCoroutineTask(static_cast<size_t>(-1), [](){}),
                      std::length_error);
    BOOST_CHECK_THROW(
        g_scheduler->RegistCoroutineTask(
            static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max()), [](){}),
        std::length_error);

    bool succ = true;
    g_scheduler->RegistCoroutineTask(size_t(0), [](){}, succ);
    BOOST_CHECK(!succ);
    succ = true;
    g_scheduler->RegistCoroutineTask(static_cast<size_t>(-1), [](){}, succ);
    BOOST_CHECK(!succ);

    /* 真实任务仍可注册（拒绝没有破坏运行时） */
    bbt::core::thread::CountDownLatch done{1};
    g_scheduler->RegistCoroutineTask([&]() { done.Down(); });
    BOOST_CHECK_EQUAL(done.WaitTimeout(3000), 0);
}

BOOST_AUTO_TEST_SUITE_END()
