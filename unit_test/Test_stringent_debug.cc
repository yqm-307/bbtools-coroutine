#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <mutex>

/* #404：STRINGENT_DEBUG 诊断调用按真实借用所有权上报（this，非
 * shared_from_this）。本测试只做行为冒烟：在宏开启的构建中驱动全部
 * 诊断点（Resume、Yield、YieldWithCallback、YieldAndPushGCoQueue、
 * CommitYield 终态核销、CHECK_FAILED 重入补登记）走真实配对路径，
 * DebugMgr 的 repeat-resume / repeat-yield / repeat-regist / trigger
 * 断言不触发即通过；宏关闭时同一路径照常运行，行为用例不为空转。
 * 诊断内容本身的场景验收属 #384，不在此票宣称。 */

#include <atomic>
#include <memory>
#include <stdexcept>
#include <unistd.h>
#include <bbt/coroutine/detail/CoPollEvent.hpp>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/debug/DebugMgr.hpp>

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

/* 析构探针：以值捕获进协程 lambda，随 Coroutine 对象（其 Context::
 * m_user_main 持有 lambda target）一起析构。latch Down 即「该协程已被
 * Processer delete」的因果证据——Processer 的顺序是 CommitYield（终态
 * 核销点）在前、delete 在后，因此 latch 放行后 Check_IsResumedCo 的
 * 检查时序严格成立，不依赖任何 sleep 窗口。
 * 用法注意：调用方持有的那份 shared_ptr 必须 reset() 之后再 Wait()，
 * 否则测试自身占用最后一引用，探针永不析构。 */
struct DestructionProbe
{
    explicit DestructionProbe(bbt::core::thread::CountDownLatch* l): latch(l) {}
    ~DestructionProbe() { latch->Down(); }
    bbt::core::thread::CountDownLatch* latch;
};
}

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(StringentDebugTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    auto* cfg = g_bbt_coroutine_config.get();
    cfg->m_cfg_static_thread_num = 1;
    cfg->m_cfg_stack_size = 8192;
    cfg->m_cfg_stack_protect = false;
    EnsureRuntime();
}

/* YieldAndPushGCoQueue ↔ Resume 配对：bbtco_yield 挂起入全局就绪队列，
 * 之后被同一/其它 worker 再次 Resume；多轮覆盖重复配对，DebugMgr
 * m_co_map 的重复登记/遗漏断言不触发。 */
BOOST_AUTO_TEST_CASE(t_ready_yield_resume_pairs)
{
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int rounds{0};

    bbtco [&]() {
        for (int i = 0; i < 8; ++i) {
            bbtco_yield;
            rounds.fetch_add(1);
        }
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(rounds.load(), 8);
}

/* YieldWithCallback ↔ Resume + 事件 regist/trigger 配对：bbtco_sleep
 * 走 YieldUntilTimeout → YieldWithCallback → _RegistAwaitEvent →
 * OnEvent_RegistEvent，唤醒走 OnCoPollEvent → OnEvent_TriggerEvent。 */
BOOST_AUTO_TEST_CASE(t_event_wait_resume_pairs)
{
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int wakes{0};

    bbtco [&]() {
        for (int i = 0; i < 4; ++i) {
            bbtco_sleep(10);
            wakes.fetch_add(1);
        }
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(wakes.load(), 4);
}

/* 并发协程交错挂起/恢复：多协程在多个事件等待间交错，m_co_map 以
 * CoroutineId 为键，互不串扰；任一配对失衡立即由 AssertWithInfo 暴露。 */
BOOST_AUTO_TEST_CASE(t_concurrent_cos_independent_pairs)
{
    constexpr int kCoNum = 4;
    bbt::core::thread::CountDownLatch done{kCoNum};
    std::atomic_int finished{0};

    for (int i = 0; i < kCoNum; ++i) {
        bbtco [&, i]() {
            for (int j = 0; j <= i; ++j)
                bbtco_sleep(5 + i * 5);
            finished.fetch_add(1);
            done.Down();
        };
    }

    done.Wait();
    BOOST_CHECK_EQUAL(finished.load(), kCoNum);
}

/* 诊断配对收口——正常 FINAL：协程终态的最后一次让出走
 * Context::_CoroutineMain 尾部 → Context::Yield，不经过
 * Coroutine::Yield，m_co_map 的核销点在 CommitYield 的 FINAL 分支。
 * 探针 latch 证明 delete 已完成（CommitYield 先于 delete），此时
 * Check_IsResumedCo 必须观察到无残留。宏关闭时同样真实跑通。 */
BOOST_AUTO_TEST_CASE(t_final_coroutine_not_stuck_as_resumed)
{
    bbt::core::thread::CountDownLatch destroyed{1};
    std::atomic<CoroutineId> seen{BBT_COROUTINE_INVALID_COROUTINE_ID};
    auto probe = std::make_shared<DestructionProbe>(&destroyed);

    bbtco [&, probe]() {
        seen.store(g_bbt_tls_coroutine_co->GetId());
    };  // 协程体结束走 FINAL；探针随 Coroutine 析构 Down latch

    probe.reset();  // 释放测试侧引用，delete 协程时探针成为最后所有者
    destroyed.Wait();
    BOOST_REQUIRE(seen.load() != BBT_COROUTINE_INVALID_COROUTINE_ID);
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->Check_IsResumedCo(seen.load());   // 残留则断言中止
#endif
    BOOST_TEST(true);
}

/* 异常 FINAL：协程体抛出未处理异常，Context catch → OnException 置
 * CO_FINAL 后同样走尾部 Context::Yield；终态核销必须与正常路径一致。 */
BOOST_AUTO_TEST_CASE(t_exception_final_not_stuck_as_resumed)
{
    bbt::core::thread::CountDownLatch destroyed{1};
    std::atomic<CoroutineId> seen{BBT_COROUTINE_INVALID_COROUTINE_ID};
    auto probe = std::make_shared<DestructionProbe>(&destroyed);

    bbtco [&, probe]() {
        seen.store(g_bbt_tls_coroutine_co->GetId());
        throw std::runtime_error{"stringent-debug probe exception"};
    };

    probe.reset();
    destroyed.Wait();
    BOOST_REQUIRE(seen.load() != BBT_COROUTINE_INVALID_COROUTINE_ID);
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->Check_IsResumedCo(seen.load());
#endif
    BOOST_TEST(true);
}

/* CHECK_FAILED 重入配对：注入 _TestFailNextAwaitRegist 使下一次
 * _RegistAwaitEvent 真实失败 → YieldWithCallback 返回 -1，协程被
 * Context::_Resume 直接重入（绕过 Coroutine::Resume 的 Resume 记账）。
 * 补偿路径必须在协程侧补登记，否则本用例中「失败后再次 bbtco_sleep
 * + 终态」产生的 Yield 会因无配对项触发 repeat-yield 断言。 */
BOOST_AUTO_TEST_CASE(t_regist_fail_reentry_pairs)
{
    bbt::core::thread::CountDownLatch destroyed{1};
    std::atomic<CoroutineId> seen{BBT_COROUTINE_INVALID_COROUTINE_ID};
    std::atomic_int fail_ret{-2};
    auto probe = std::make_shared<DestructionProbe>(&destroyed);

    bbtco [&, probe]() {
        seen.store(g_bbt_tls_coroutine_co->GetId());
        Coroutine::_TestFailNextAwaitRegist();
        fail_ret.store(g_bbt_tls_coroutine_co->YieldUntilTimeout(5));
        /* 失败重入后再次真实挂起 + 正常唤醒 + 终态退出，覆盖
         * 补登记 → Yield 核销 → Resume → 终态核销 全链。 */
        bbtco_sleep(5);
    };

    probe.reset();
    destroyed.Wait();
    BOOST_CHECK_EQUAL(fail_ret.load(), -1);
    BOOST_REQUIRE(seen.load() != BBT_COROUTINE_INVALID_COROUTINE_ID);
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->Check_IsResumedCo(seen.load());
#endif
    BOOST_TEST(true);
}

/* 直接驱动 FINAL 清账——正常路径：Create/Resume/delete 不经过 Processer，
 * 终态让出走 Context::_CoroutineMain 尾部 Context::Yield，无 CommitYield
 * 核销点；登记残留必须由 ~Coroutine 的共同出口幂等清账，delete 后
 * Check_IsResumedCo 不得观察到借用记录（F4 回归：此前此路径残留致
 * SIGABRT）。测试线程直接 Resume 属既有合法用法（见 Test_coroutine.cc）。 */
BOOST_AUTO_TEST_CASE(t_direct_drive_final_cleanup)
{
    auto* co = Coroutine::Create(64 * 1024, [](){}, false);
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    const CoroutineId id = co->GetId();
#endif
    co->Resume();
    BOOST_REQUIRE(co->GetStatus() == CoroutineStatus::CO_FINAL);
    delete co;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->Check_IsResumedCo(id);   // 残留则断言中止
#endif
    BOOST_TEST(true);
}

/* 直接驱动 FINAL 清账——异常路径：协程体抛出 → Context catch →
 * OnException 置 CO_FINAL → 同样尾部让出；delete 必须同样清账。 */
BOOST_AUTO_TEST_CASE(t_direct_drive_exception_final_cleanup)
{
    const auto before = g_bbt_coroutine_config->m_unhandled_exception_count.load();
    auto* co = Coroutine::Create(64 * 1024, [](){
        throw std::runtime_error{"direct drive final probe"};
    }, false);
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    const CoroutineId id = co->GetId();
#endif
    co->Resume();
    BOOST_REQUIRE(co->GetStatus() == CoroutineStatus::CO_FINAL);
    BOOST_CHECK_EQUAL(g_bbt_coroutine_config->m_unhandled_exception_count.load(), before + 1);
    delete co;
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->Check_IsResumedCo(id);
#endif
    BOOST_TEST(true);
}

/* fd 建立后、登记前发生 close claim：拒绝的等待同样必须有诊断登记，
 * 其 CLOSED 提前触发在 CommitPark 消费；不靠线程时序或新 seam。 */
BOOST_AUTO_TEST_CASE(t_fd_rejected_registration_pairs)
{
    struct PipeGuard {
        int fd[2]{-1, -1};
        ~PipeGuard() { for (int value : fd) if (value >= 0) ::close(value); }
    } pipe;
    BOOST_REQUIRE_EQUAL(::pipe(pipe.fd), 0);
    std::atomic_int completed{0}, flags{0};
    auto event = CoPollEvent::Create(BBT_COROUTINE_INVALID_COROUTINE_ID,
        [&](auto, int result, int) { flags.store(result); completed.fetch_add(1); });
    BOOST_REQUIRE_EQUAL(event->InitFdEvent(pipe.fd[0], bbt::pollevent::EventOpt::READABLE, 0), 0);
    BOOST_REQUIRE(CoPollEvent::BeginFdClose(pipe.fd[0]));
    struct CloseClaimGuard {
        int fd;
        ~CloseClaimGuard() { CoPollEvent::EndFdClose(fd); }
    } claim{pipe.fd[0]};
    BOOST_REQUIRE_EQUAL(event->Regist(), 0);
    BOOST_REQUIRE(event->CommitPark());
    BOOST_CHECK_EQUAL(completed.load(), 1);
    BOOST_CHECK((flags.load() & POLL_EVENT_CLOSED) != 0);
}

BOOST_AUTO_TEST_CASE(t_end)
{
}

BOOST_AUTO_TEST_SUITE_END()
