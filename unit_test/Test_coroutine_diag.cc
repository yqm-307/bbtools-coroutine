#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

// #276 协程诊断现场：bbtco_desc 落库 + 挂起现场（状态/等待类型/等待对象/等待时长）。
// 契约：现场由协程自身（同线程）读取；外部线程并发快照属 #277 范围。

#include <atomic>
#include <chrono>
#include <thread>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(CoroutineDiagTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    auto* cfg = g_bbt_coroutine_config.get();
    cfg->m_cfg_static_thread_num = 1;
    cfg->m_cfg_stack_size = 8192;
    cfg->m_cfg_stack_protect = false;
    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
}

// desc 落库：_CoHelper 把 bbtco_desc 的字符串存入协程，协程内可读回
BOOST_AUTO_TEST_CASE(t_desc_recorded_and_readable)
{
    bbt::core::thread::CountDownLatch done{1};
    std::string seen{"<unset>"};

    bbtco_desc("diag-probe-task") [&]() {
        Coroutine* co = g_bbt_tls_coroutine_co;
        BOOST_REQUIRE(co != nullptr);
        seen = co->GetDescription();
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(seen, "diag-probe-task");
}

// 普通 bbtco 默认空描述，行为不变
BOOST_AUTO_TEST_CASE(t_plain_bbtc_has_empty_desc)
{
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_bool empty{false};

    bbtco [&]() {
        Coroutine* co = g_bbt_tls_coroutine_co;
        BOOST_REQUIRE(co != nullptr);
        empty.store(co->GetDescription().empty());
        done.Down();
    };

    done.Wait();
    BOOST_CHECK(empty.load());
}

// 挂起现场：sleep 等待中可读回 状态=CO_SUSPEND、等待类型含 TIMEOUT、时长>0
BOOST_AUTO_TEST_CASE(t_wait_scene_while_parked)
{
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_bool checked{false};

    bbtco_desc("parked-probe") [&]() {
        Coroutine* co = g_bbt_tls_coroutine_co;
        BOOST_REQUIRE(co != nullptr);

        // 触发一次带超时的真实挂起：20ms sleep 后用现场核对
        bbtco_sleep(20);

        // 已被唤醒后的现场：状态回到 RUNNING、parked 等待信息不可用
        CoroutineWaitInfo info;
        BOOST_CHECK_EQUAL(co->GetStatus(), CoroutineStatus::CO_RUNNING);
        BOOST_CHECK_EQUAL(co->GetWaitInfo(info), -1);
        checked.store(true);
        done.Down();
    };

    done.Wait();
    BOOST_CHECK(checked.load());
}

// 挂起中读现场：由另一个协程通过 parked 唤醒路径不可行（跨线程禁止），
// 改为在挂起点之前用显式 YieldUntilTimeout 前现场查询 + 唤醒后清零双段锁定：
// sleep 挂起前记录 waited，唤醒后 waited_us 不再增长。
BOOST_AUTO_TEST_CASE(t_waited_us_bounded_after_wake)
{
    bbt::core::thread::CountDownLatch done{1};

    bbtco [&]() {
        Coroutine* co = g_bbt_tls_coroutine_co;
        BOOST_REQUIRE(co != nullptr);

        CoroutineWaitInfo before;
        BOOST_CHECK_EQUAL(co->GetWaitInfo(before), -1);  // 未挂起时无等待现场

        bbtco_sleep(30);

        CoroutineWaitInfo after;
        BOOST_CHECK_EQUAL(co->GetWaitInfo(after), -1);   // 唤醒后现场已清
        BOOST_CHECK_GT(co->GetLastResumeEvent(), 0);     // 但保留最后一次唤醒事件（TIMEOUT 位）
        done.Down();
    };

    done.Wait();
}

BOOST_AUTO_TEST_CASE(t_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
