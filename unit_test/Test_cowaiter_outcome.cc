/**
 * @file Test_cowaiter_outcome.cc
 * @brief #347 切片② 验收：唤醒原因独立化（取消位不再借用超时位）+
 *        CoWaiter 窄接口 Wait(WaitOptions)→WaitStatus 的单次等待结果收口。
 *
 * 确定性手段：以等待协程进入 CO_SUSPEND 为屏障（观察到即说明等待事件
 * 已建好，触发必走事件状态机 PENDING/PARKED 路径）；提前完成由
 * 自旋 Notify 覆盖 INITED/ARMED/PARKED 各落点。禁止 sleep 凑时序、
 * 禁止空断言：除结果状态外同时断言 GetLastResumeEvent() 的真实掩码位。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

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
#include <bbt/coroutine/sync/Cancellation.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

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

/* 协程已进入事件挂起：CO_SUSPEND 置位先于挂起回调内的事件登记；
 * 观察到即说明等待事件对象已建好，此后 Notify/取消必走状态机路径。 */
bool CoroutineSuspended(std::atomic<Coroutine*>& co)
{
    auto* p = co.load();
    return p != nullptr && p->GetStatus() == CoroutineStatus::CO_SUSPEND;
}

/* 自旋 Notify 直到成功或有界放弃；用于覆盖「登记前完成」的落点窗口 */
bool NotifyUntilSuccess(const sync::CoWaiter::SPtr& waiter, int budget_ms = 10000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (waiter->Notify() != 0) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

}

BOOST_AUTO_TEST_SUITE(CoWaiterOutcomeTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    auto* cfg = g_bbt_coroutine_config.get();
    if (!g_cfg.saved) {
        g_cfg.threads = cfg->m_cfg_static_thread_num;
        cfg->m_cfg_static_thread_num = 1;
        cfg->m_cfg_stack_protect = false;
        g_cfg.saved = true;
    }

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    g_started.store(true);
    BOOST_REQUIRE(g_scheduler->IsRunning());
}

/* 1. 唤醒原因三向可区分：真超时→TimedOut(TIMEOUT 位)、协程级取消→
 * Cancelled(CANCELLED 位)、Notify→Completed(CUSTOM 位)；三者掩码互不混淆 */
BOOST_AUTO_TEST_CASE(t_resume_reason_cancel_is_distinct_from_timeout)
{
    /* (a) 真超时：定时器到点唤醒，掩码只含 TIMEOUT */
    {
        auto waiter = sync::CoWaiter::Create();
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<WaitStatus> st{};
        std::atomic<int> mask{-1};

        WaitOptions opt;
        opt.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
        bbtco [&]() {
            st.store(waiter->Wait(opt));
            mask.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
            done.Down();
        };
        done.Wait();
        BOOST_CHECK(st.load() == WaitStatus::TimedOut);
        BOOST_CHECK(mask.load() & POLL_EVENT_TIMEOUT);
        BOOST_CHECK(!(mask.load() & POLL_EVENT_CANCELLED));
        BOOST_CHECK(!(mask.load() & POLL_EVENT_CUSTOM));
    }

    /* (b) 协程级取消：掩码只含 CANCELLED，不再借 TIMEOUT */
    {
        auto waiter = sync::CoWaiter::Create();
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<Coroutine*> co_p{nullptr};
        std::atomic<WaitStatus> st{};
        std::atomic<int> mask{-1};

        bbtco [&]() {
            co_p.store(g_bbt_tls_coroutine_co);
            st.store(waiter->Wait(WaitOptions{}));    /* 无期限：醒即取消 */
            mask.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
            done.Down();
        };
        BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
        co_p.load()->RequestCancel();
        done.Wait();
        BOOST_CHECK(st.load() == WaitStatus::Cancelled);
        BOOST_CHECK(mask.load() & POLL_EVENT_CANCELLED);
        BOOST_CHECK(!(mask.load() & POLL_EVENT_TIMEOUT));
        BOOST_CHECK(!(mask.load() & POLL_EVENT_CUSTOM));
    }

    /* (c) Notify 完成：掩码只含 CUSTOM */
    {
        auto waiter = sync::CoWaiter::Create();
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<Coroutine*> co_p{nullptr};
        std::atomic<WaitStatus> st{};
        std::atomic<int> mask{-1};

        bbtco [&]() {
            co_p.store(g_bbt_tls_coroutine_co);
            st.store(waiter->Wait(WaitOptions{}));
            mask.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
            done.Down();
        };
        BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
        BOOST_CHECK_EQUAL(waiter->Notify(), 0);
        done.Wait();
        BOOST_CHECK(st.load() == WaitStatus::Completed);
        BOOST_CHECK(mask.load() & POLL_EVENT_CUSTOM);
        BOOST_CHECK(!(mask.load() & POLL_EVENT_CANCELLED));
        BOOST_CHECK(!(mask.load() & POLL_EVENT_TIMEOUT));
    }
}

/* 2. 登记前完成不丢唤醒：Notify 最早的合法触发点在事件已建未注册
 * （INITED），由 PENDING 吸收；各轮落点不同但决议必须恒为 Completed */
BOOST_AUTO_TEST_CASE(t_register_before_complete_no_lost_wakeup)
{
    for (int i = 0; i < 200; ++i) {
        auto waiter = sync::CoWaiter::Create();
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<WaitStatus> st{};

        bbtco [&]() {
            st.store(waiter->Wait(WaitOptions{}));
            done.Down();
        };

        std::atomic_bool notified{false};
        std::thread notifier([&]() {
            notified.store(NotifyUntilSuccess(waiter));
        });

        /* 唤醒丢失时 bounded 失败而非挂死整个用例 */
        BOOST_REQUIRE_MESSAGE(done.WaitTimeout(5000) == 0,
            "round " << i << "：等待未在有界时间内返回（notified=" << notified.load() << ")");
        notifier.join();
        BOOST_CHECK_MESSAGE(st.load() == WaitStatus::Completed,
            "round " << i << "：登记前完成丢了通知");
    }
}

/* 3. 超时先胜出、完成晚到不改判：第一次等待固定为 TimedOut；
 * 晚到 Notify 打在已 FINAL 的事件上是 no-op，下一次等待不受影响 */
BOOST_AUTO_TEST_CASE(t_timeout_wins_then_complete_stays_timedout)
{
    auto waiter = sync::CoWaiter::Create();
    bbt::core::thread::CountDownLatch first_done{1}, done{1};
    std::atomic<WaitStatus> st1{}, st2{};
    std::atomic<int> mask1{-1};

    WaitOptions opt;
    opt.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);

    bbtco [&]() {
        st1.store(waiter->Wait(opt));
        mask1.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
        first_done.Down();
        st2.store(waiter->Wait(WaitOptions{}));
        done.Down();
    };

    first_done.Wait();
    BOOST_CHECK(st1.load() == WaitStatus::TimedOut);
    BOOST_CHECK(mask1.load() & POLL_EVENT_TIMEOUT);
    BOOST_CHECK(!(mask1.load() & POLL_EVENT_CANCELLED));

    /* 完成晚到：上一次等待已决议，不得改判。此次 Notify 可能落在已清空的
     * 等待位（no-op），也可能恰好唤醒已登记的第二次等待——两种时序下
     * st1 的决议都不变，st2 均应为 Completed。 */
    waiter->Notify();

    /* 自旋随完成退出：若上面的晚到 Notify 已唤醒第二次等待，不得空转 */
    std::atomic_bool stop_spin{false};
    std::thread notifier([&]() {
        while (!stop_spin.load(std::memory_order_acquire) && waiter->Notify() != 0)
            std::this_thread::yield();
    });
    done.Wait();
    stop_spin.store(true, std::memory_order_release);
    notifier.join();
    BOOST_CHECK(st2.load() == WaitStatus::Completed);
}

/* 4. 旧订阅晚到：第一轮等待退出后取消令牌才置位——登记已随 Wait 返回
 * 解绑、旧事件已 FINAL，晚到的取消不得误唤醒或影响下一次等待 */
BOOST_AUTO_TEST_CASE(t_late_subscription_after_wait_returns)
{
    auto waiter = sync::CoWaiter::Create();
    CancellationSource source;

    bbt::core::thread::CountDownLatch first_done{1}, done{1};
    std::atomic<Coroutine*> co_p{nullptr};
    std::atomic<WaitStatus> st1{}, st2{}, st3{};

    WaitOptions opt1;
    opt1.cancel = source.Token();

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        st1.store(waiter->Wait(opt1));
        first_done.Down();
        /* 第二轮不带令牌：若旧订阅残留，任何晚到回调都可能误伤本轮 */
        st2.store(waiter->Wait(WaitOptions{}));
        /* 第三轮带同一已取消令牌：验证令牌路径本身仍正确工作 */
        WaitOptions opt3;
        opt3.cancel = source.Token();
        st3.store(waiter->Wait(opt3));
        done.Down();
    };

    /* 第一轮：令牌取消挂起中被正常 Notify 完成 */
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
    BOOST_CHECK_EQUAL(waiter->Notify(), 0);
    first_done.Wait();
    BOOST_CHECK(st1.load() == WaitStatus::Completed);

    /* 等待已返回后令牌才取消：登记已解绑，晚到订阅即便在途也只打在
     * FINAL 事件上（Trigger 终态 no-op） */
    source.RequestCancel();

    /* 第二轮：必须仍由 Notify 正常完成——晚到取消没有污染等待位。
     * 自旋随全部三轮结束退出：第三轮走入口取消预检不挂起，无需 Notify。 */
    std::atomic_bool stop_spin{false};
    std::thread notifier([&]() {
        while (!stop_spin.load(std::memory_order_acquire) && waiter->Notify() != 0)
            std::this_thread::yield();
    });
    done.Wait();
    stop_spin.store(true, std::memory_order_release);
    notifier.join();
    BOOST_CHECK(st2.load() == WaitStatus::Completed);
    BOOST_CHECK(st3.load() == WaitStatus::Cancelled);
}

/* 5. 旧入口行为逐条不变：正常唤醒 / 真超时 / 协程级取消三种情形 */
BOOST_AUTO_TEST_CASE(t_legacy_waiter_behavior_unchanged)
{
    enum class Kind { Wait, WaitCb, Timeout, TimeoutCb };
    const Kind kinds[] = { Kind::Wait, Kind::WaitCb, Kind::Timeout, Kind::TimeoutCb };
    /* 取消借用超时位时期的返回值（改动前）：Wait 族 0，超时族 1 */
    const int on_notify[] = { 0, 0, 0, 0 };
    const int on_cancel[] = { 0, 0, 1, 1 };

    /* 正常唤醒：Notify → 全 0 */
    for (int i = 0; i < 4; ++i) {
        auto waiter = sync::CoWaiter::Create();
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<Coroutine*> co_p{nullptr};
        std::atomic<int> ret{-99};

        bbtco [&, i]() {
            co_p.store(g_bbt_tls_coroutine_co);
            int r = -99;
            switch (kinds[i]) {
            case Kind::Wait:      r = waiter->Wait(); break;
            case Kind::WaitCb:    r = waiter->WaitWithCallback([] { return true; }); break;
            case Kind::Timeout:   r = waiter->WaitWithTimeout(60000); break;
            case Kind::TimeoutCb: r = waiter->WaitWithTimeoutAndCallback(60000, [] { return true; }); break;
            }
            ret.store(r);
            done.Down();
        };
        BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
        BOOST_CHECK_EQUAL(waiter->Notify(), 0);
        done.Wait();
        BOOST_CHECK_EQUAL(ret.load(), on_notify[i]);
    }

    /* 真超时：超时族 → 1 */
    for (int i = 2; i < 4; ++i) {
        auto waiter = sync::CoWaiter::Create();
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<int> ret{-99};

        bbtco [&, i]() {
            int r = -99;
            if (kinds[i] == Kind::Timeout)
                r = waiter->WaitWithTimeout(60);
            else
                r = waiter->WaitWithTimeoutAndCallback(60, [] { return true; });
            ret.store(r);
            done.Down();
        };
        done.Wait();
        BOOST_CHECK_EQUAL(ret.load(), 1);
    }

    /* 协程级取消：Wait 族 → 0，超时族 → 1（与借用超时位时期一致） */
    for (int i = 0; i < 4; ++i) {
        auto waiter = sync::CoWaiter::Create();
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<Coroutine*> co_p{nullptr};
        std::atomic<int> ret{-99};

        bbtco [&, i]() {
            co_p.store(g_bbt_tls_coroutine_co);
            int r = -99;
            switch (kinds[i]) {
            case Kind::Wait:      r = waiter->Wait(); break;
            case Kind::WaitCb:    r = waiter->WaitWithCallback([] { return true; }); break;
            case Kind::Timeout:   r = waiter->WaitWithTimeout(60000); break;
            case Kind::TimeoutCb: r = waiter->WaitWithTimeoutAndCallback(60000, [] { return true; }); break;
            }
            ret.store(r);
            done.Down();
        };
        BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
        co_p.load()->RequestCancel();
        done.Wait();
        BOOST_CHECK_EQUAL(ret.load(), on_cancel[i]);
    }
}

/* 6. 令牌取消与完成在窄接口上可区分 */
BOOST_AUTO_TEST_CASE(t_token_cancel_distinct_from_completed)
{
    /* (a) 挂起中令牌取消 → Cancelled，掩码为 CANCELLED */
    {
        auto waiter = sync::CoWaiter::Create();
        CancellationSource source;
        WaitOptions opt;
        opt.cancel = source.Token();

        bbt::core::thread::CountDownLatch done{1};
        std::atomic<Coroutine*> co_p{nullptr};
        std::atomic<WaitStatus> st{};
        std::atomic<int> mask{-1};

        bbtco [&]() {
            co_p.store(g_bbt_tls_coroutine_co);
            st.store(waiter->Wait(opt));
            mask.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
            done.Down();
        };
        BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
        source.RequestCancel();
        done.Wait();
        BOOST_CHECK(st.load() == WaitStatus::Cancelled);
        BOOST_CHECK(mask.load() & POLL_EVENT_CANCELLED);
        BOOST_CHECK(!(mask.load() & POLL_EVENT_CUSTOM));
        BOOST_CHECK(!(mask.load() & POLL_EVENT_TIMEOUT));
    }

    /* (b) 同一形态等待由 Notify 完成 → Completed，掩码为 CUSTOM */
    {
        auto waiter = sync::CoWaiter::Create();
        CancellationSource source;
        WaitOptions opt;
        opt.cancel = source.Token();

        bbt::core::thread::CountDownLatch done{1};
        std::atomic<Coroutine*> co_p{nullptr};
        std::atomic<WaitStatus> st{};
        std::atomic<int> mask{-1};

        bbtco [&]() {
            co_p.store(g_bbt_tls_coroutine_co);
            st.store(waiter->Wait(opt));
            mask.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
            done.Down();
        };
        BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
        BOOST_CHECK_EQUAL(waiter->Notify(), 0);
        done.Wait();
        BOOST_CHECK(st.load() == WaitStatus::Completed);
        BOOST_CHECK(mask.load() & POLL_EVENT_CUSTOM);
        BOOST_CHECK(!(mask.load() & POLL_EVENT_CANCELLED));
    }
}

/* 补充：窄接口的 AlreadyWaiting / InvalidContext / 已过期 deadline */
BOOST_AUTO_TEST_CASE(t_narrow_wait_rejections)
{
    auto waiter = sync::CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<Coroutine*> co_p{nullptr};
    std::atomic<WaitStatus> st_a{}, st_b{}, st_expired{};

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        st_a.store(waiter->Wait(WaitOptions{}));
        done.Down();
    };
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));

    /* 并发第二个等待者 → AlreadyWaiting */
    bbt::core::thread::CountDownLatch b_done{1};
    bbtco [&]() {
        st_b.store(waiter->Wait(WaitOptions{}));
        b_done.Down();
    };
    b_done.Wait();
    BOOST_CHECK(st_b.load() == WaitStatus::AlreadyWaiting);

    BOOST_CHECK_EQUAL(waiter->Notify(), 0);
    done.Wait();
    BOOST_CHECK(st_a.load() == WaitStatus::Completed);

    /* 已过期 deadline → TimedOut，不发生挂起 */
    WaitOptions expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    bbt::core::thread::CountDownLatch e_done{1};
    bbtco [&]() {
        st_expired.store(waiter->Wait(expired));
        e_done.Down();
    };
    e_done.Wait();
    BOOST_CHECK(st_expired.load() == WaitStatus::TimedOut);

    /* 普通线程 → InvalidContext */
    BOOST_CHECK(waiter->Wait(WaitOptions{}) == WaitStatus::InvalidContext);
}

BOOST_AUTO_TEST_CASE(t_end)
{
    if (g_started.exchange(false))
        g_scheduler->Stop();

    if (g_cfg.saved) {
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = g_cfg.threads;
        cfg->m_cfg_stack_protect = g_cfg.stack_protect;
        g_cfg.saved = false;
    }
}

BOOST_AUTO_TEST_SUITE_END()
