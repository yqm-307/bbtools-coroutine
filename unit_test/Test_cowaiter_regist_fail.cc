/**
 * @file Test_cowaiter_regist_fail.cc
 * @brief #369 回归：旧超时族接口在「事件登记失败、协程未真正挂起」时不得
 *        读取上一次唤醒原因掩码把失败误报为超时/取消。
 *
 * 注入手段：Coroutine::_TestFailNextAwaitRegist() 置位后，下一次
 * _RegistAwaitEvent() 先把已创建的 await 事件 UnRegist 成 CANCELLED，
 * 再走真实 Regist() → -1 → YieldWithCallback 返回 -1。失败发生在事件
 * 注册阶段（RegistCustom 已成功返回），不是创建阶段。
 *
 * 区分假设的设计：先让同一协程真实超时一次，把 m_last_resume_event
 * 固定为 POLL_EVENT_TIMEOUT；再注入登记失败。若旧接口仍返回 1，说明
 * 读的是陈旧掩码（BUG）；返回 -1 才符合「未挂起即失败」契约。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <mutex>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

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
    bool stack_protect{false};
    bool saved{false};
} g_cfg;

std::atomic_bool g_started{false};

/* 确定性屏障：自旋等待真实条件成立（非定时 sleep），有界防挂死。 */
bool WaitUntil(const std::function<bool()>& pred, int budget_ms = 10000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

}

BOOST_AUTO_TEST_SUITE(CoWaiterRegistFailTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    auto* cfg = g_bbt_coroutine_config.get();
    if (!g_cfg.saved) {
        g_cfg.threads = cfg->m_cfg_static_thread_num;
        cfg->m_cfg_static_thread_num = 1;     /* 单 worker：协程内部顺序即确定 */
        cfg->m_cfg_stack_protect = false;
        g_cfg.saved = true;
    }

    EnsureRuntime();
    g_started.store(true);
    BOOST_REQUIRE(g_scheduler->IsInitialized());
}

/* 1. 注入确实落在事件注册阶段而非创建阶段：
 *    RegistCustom 已建好事件（等待位已占），随后 _RegistAwaitEvent 失败，
 *    YieldWithCallback 返回 -1，且唤醒掩码未被本次失败刷新。 */
BOOST_AUTO_TEST_CASE(t_regist_fail_keeps_stale_resume_mask)
{
    auto waiter = sync::CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<int> first_ret{-99}, second_ret{-99}, mask_after{-99};

    bbtco [&]() {
        /* 第一次真实超时：把 m_last_resume_event 固定为 TIMEOUT */
        first_ret.store(waiter->WaitWithTimeout(50));
        /* 下一次带 await 事件的登记必定失败：真实 Regist() 走到 -1 */
        Coroutine::_TestFailNextAwaitRegist();
        second_ret.store(waiter->WaitWithTimeout(60000));
        mask_after.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(first_ret.load(), 1);                 /* 真超时 → 1，掩码已置位 */
    BOOST_CHECK_EQUAL(second_ret.load(), -1);               /* 登记失败 → -1，不得报超时 */
    /* 失败不刷新掩码：仍是上一轮的 TIMEOUT。若实现误读旧值，此处暴露污染源 */
    BOOST_CHECK(mask_after.load() & POLL_EVENT_TIMEOUT);
}

/* 2. 同注入下三组对照：
 *    - WaitWithTimeout          ：修复目标，必须 -1；
 *    - WaitWithTimeoutAndCallback：既有 ret==0 守卫，基线即 -1（回归对照）；
 *    - Wait(WaitOptions) 窄接口  ：契约 RuntimeUnavailable（回归对照）。 */
BOOST_AUTO_TEST_CASE(t_regist_fail_return_codes_across_interfaces)
{
    auto waiter = sync::CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<int> r_plain{-99}, r_cb{-99};
    std::atomic<WaitStatus> r_narrow{WaitStatus::Completed};

    bbtco [&]() {
        /* 先污染掩码：真实超时把 m_last_resume_event 固定为 TIMEOUT。
         * 不用 RequestCancel——取消位是粘性的，会让后续等待走预取消
         * 完成路径而非登记失败路径，干扰注入。 */
        int warm = waiter->WaitWithTimeout(50);
        BOOST_ASSERT(warm == 1);

        Coroutine::_TestFailNextAwaitRegist();
        r_plain.store(waiter->WaitWithTimeout(60000));

        Coroutine::_TestFailNextAwaitRegist();
        r_cb.store(waiter->WaitWithTimeoutAndCallback(60000, [] { return true; }));

        Coroutine::_TestFailNextAwaitRegist();
        r_narrow.store(waiter->Wait(WaitOptions{}));

        done.Down();
    };

    done.Wait();
    BOOST_CHECK_EQUAL(r_plain.load(), -1);                  /* #369 修复点 */
    BOOST_CHECK_EQUAL(r_cb.load(), -1);                     /* 既有守卫保持一致 */
    BOOST_CHECK(r_narrow.load() == WaitStatus::RuntimeUnavailable);
}

/* 3. 登记失败后等待位正确解绑：下一次正常等待不受影响（状态清理回归）。 */
BOOST_AUTO_TEST_CASE(t_waiter_usable_after_regist_fail)
{
    auto waiter = sync::CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<int> r_fail{-99}, r_ok{-99};

    bbtco [&]() {
        Coroutine::_TestFailNextAwaitRegist();
        r_fail.store(waiter->WaitWithTimeout(60000));
        /* 失败后等待位须已释放：立即再等待并由 Notify 正常完成 */
        r_ok.store(waiter->WaitWithTimeout(60000));
        done.Down();
    };

    BOOST_REQUIRE(WaitUntil([&]() { return r_fail.load() == -1; }));
    /* 自旋 Notify：等待位可能尚未登记完成，早期失败由循环吸收 */
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (waiter->Notify() != 0) {
        if (std::chrono::steady_clock::now() > deadline) {
            done.WaitTimeout(0);
            break;
        }
        std::this_thread::yield();
    }
    done.Wait();
    BOOST_CHECK_EQUAL(r_fail.load(), -1);
    BOOST_CHECK_EQUAL(r_ok.load(), 0);
}

/* 4. （已收窄移除）CoMutex::TryLock(ms) 登记失败时的「不死于重入
 *    _SysLock」依赖 _SysUnLock 补偿——那是 Issue #369 明确排除的 CoMutex
 *    锁内部死锁修复，属另一 Issue 范围。本票只修返回码映射：
 *    CoMutex.cc 的 `ret == 0` 掩码守卫保证登记失败不误报超时；sys 锁
 *    不变量修复不在本票。详见 verification.md 未覆盖项。 */

/* 5. 注入标志单次消费：_TestFailNextAwaitRegist 只影响下一次登记，
 *    其后再等待走正常路径（Notify 完成返回 0）。 */
BOOST_AUTO_TEST_CASE(t_injection_is_one_shot)
{
    auto waiter = sync::CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<Coroutine*> co_p{nullptr};
    std::atomic<int> r_fail{-99}, r_ok{-99};

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        Coroutine::_TestFailNextAwaitRegist();
        r_fail.store(waiter->WaitWithTimeout(60000));
        r_ok.store(waiter->WaitWithTimeout(60000));
        done.Down();
    };

    BOOST_REQUIRE(WaitUntil([&]() { return r_fail.load() == -1; }));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (waiter->Notify() != 0) {
        if (std::chrono::steady_clock::now() > deadline)
            break;
        std::this_thread::yield();
    }
    done.Wait();
    BOOST_CHECK_EQUAL(r_fail.load(), -1);
    BOOST_CHECK_EQUAL(r_ok.load(), 0);     /* 第二次等待正常完成，注入不残留 */
}

BOOST_AUTO_TEST_CASE(t_end)
{
    if (g_started.exchange(false))
    if (g_cfg.saved) {
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = g_cfg.threads;
        cfg->m_cfg_stack_protect = g_cfg.stack_protect;
        g_cfg.saved = false;
    }
}

BOOST_AUTO_TEST_SUITE_END()
