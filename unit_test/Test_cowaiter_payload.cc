/**
 * @file Test_cowaiter_payload.cc
 * @brief 带参等待 / 带载荷唤醒验证（CoWaiter 新公共面）。
 *
 * 覆盖：挂起后回调内同步投递（PENDING 早到）、首胜载荷锁定（败方不覆盖）、
 * 定时器首胜不返回败方载荷、无参 Notify 输出空值、fresh 通知失败不粘滞、
 * 登记失败不投递回调、晚到通知不误唤醒后续等待、协程级取消首胜无载荷、
 * 组合等待带载荷、错类型读取被拒绝。
 *
 * 契约依据见「coroutine process-lifetime 公共迁移」确认稿 §2 与 §4。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/sync/CoEventValue.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::sync;
using bbt::coroutine::detail::Coroutine;

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

bool WaitUntil(const std::function<bool()>& pred, int budget_ms = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

bool CoroutineSuspended(std::atomic<Coroutine*>& co)
{
    auto* p = co.load();
    return p != nullptr && p->GetStatus() == bbt::coroutine::detail::CoroutineStatus::CO_SUSPEND;
}

/* 与 int64 同尺寸、不同登记 ID：端到端错类型读取必须失败 */
struct SameSizeOtherPayload
{
    std::int64_t seq{0};
    std::uint32_t code{0};
};
} // namespace

BBT_COEVENT_VALUE_REGISTER_TYPE(SameSizeOtherPayload, 0x301);

BOOST_AUTO_TEST_SUITE(CoWaiterPayloadTest)

/* ① 登记成功后回调内同步投递：Completed 且取回同类型载荷，回调只执行一次 */
BOOST_AUTO_TEST_CASE(t_sync_notify_in_callback_delivers_payload)
{
    EnsureRuntime();
    auto waiter = CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int cb_count{0};
    std::atomic<WaitStatus> st{};
    std::atomic<std::int64_t> got{0};
    std::atomic_bool has_value{false};

    bbtco [&]() {
        CoEventValue out;
        st.store(waiter->WaitWithCallback(WaitOptions{}, [&]() {
            cb_count.fetch_add(1);
            return waiter->Notify(CoEventValue::From(std::int64_t{7})) == 0;
        }, &out));
        std::int64_t v = 0;
        if (out.Get(v))
            got.store(v);
        has_value.store(out.HasValue());
        done.Down();
    };

    BOOST_REQUIRE_EQUAL(done.WaitTimeout(5000), 0);
    BOOST_CHECK(st.load() == WaitStatus::Completed);
    BOOST_CHECK_EQUAL(cb_count.load(), 1);
    BOOST_CHECK_EQUAL(got.load(), 7);
    BOOST_CHECK(has_value.load());
}

/* ② 首胜载荷锁定：等待者恢复前连续两次 Notify，第二次失败且不覆盖赢家 */
BOOST_AUTO_TEST_CASE(t_first_win_payload_locked)
{
    EnsureRuntime();
    auto waiter = CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int second_ret{-999};
    std::atomic<WaitStatus> st{};
    std::atomic<std::int64_t> got{0};

    bbtco [&]() {
        CoEventValue out;
        st.store(waiter->WaitWithCallback(WaitOptions{}, [&]() {
            waiter->Notify(CoEventValue::From(std::int64_t{7}));
            second_ret.store(waiter->Notify(CoEventValue::From(std::int64_t{9})));
            return true;
        }, &out));
        std::int64_t v = 0;
        if (out.Get(v))
            got.store(v);
        done.Down();
    };

    BOOST_REQUIRE_EQUAL(done.WaitTimeout(5000), 0);
    BOOST_CHECK(st.load() == WaitStatus::Completed);
    BOOST_CHECK_EQUAL(second_ret.load(), -1);
    BOOST_CHECK_EQUAL(got.load(), 7);
}

/* ③ 定时器首胜：超时不得返回败方载荷，也不得残留上一次结果 */
BOOST_AUTO_TEST_CASE(t_timeout_first_win_has_no_payload)
{
    EnsureRuntime();
    auto waiter = CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<WaitStatus> st{};
    std::atomic_bool has_value{true};
    std::atomic<std::int64_t> got{-1};

    bbtco [&]() {
        WaitOptions options;
        options.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{50};
        CoEventValue out = CoEventValue::From(std::int64_t{5});   // 预置旧值
        st.store(waiter->WaitWithCallback(options, {}, &out));
        std::int64_t v = 0;
        got.store(out.Get(v) ? v : -1);
        has_value.store(out.HasValue());
        done.Down();
    };

    BOOST_REQUIRE_EQUAL(done.WaitTimeout(5000), 0);
    BOOST_CHECK(st.load() == WaitStatus::TimedOut);
    BOOST_CHECK(!has_value.load());
    BOOST_CHECK_EQUAL(got.load(), -1);
}

/* ④ 无参 Notify 唤醒带参等待：Completed，但本次通知没带载荷 */
BOOST_AUTO_TEST_CASE(t_plain_notify_gives_empty_payload)
{
    EnsureRuntime();
    auto waiter = CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<WaitStatus> st{};
    std::atomic_bool has_value{true};
    std::atomic<Coroutine*> co_p{nullptr};

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        CoEventValue out = CoEventValue::From(std::int64_t{5});
        st.store(waiter->WaitWithCallback(WaitOptions{}, {}, &out));
        has_value.store(out.HasValue());
        done.Down();
    };

    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
    BOOST_CHECK_EQUAL(waiter->Notify(), 0);
    BOOST_REQUIRE_EQUAL(done.WaitTimeout(5000), 0);
    BOOST_CHECK(st.load() == WaitStatus::Completed);
    BOOST_CHECK(!has_value.load());
}

/* ⑤ 非粘滞：fresh waiter 无等待者时带参 Notify 失败，且不留下隐藏完成位 */
BOOST_AUTO_TEST_CASE(t_fresh_notify_fails_and_not_sticky)
{
    EnsureRuntime();
    auto waiter = CoWaiter::Create();

    BOOST_CHECK_EQUAL(waiter->Notify(CoEventValue::From(std::int64_t{1})), -1);

    bbt::core::thread::CountDownLatch done{1};
    std::atomic<WaitStatus> st{};

    bbtco [&]() {
        WaitOptions options;
        options.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{50};
        CoEventValue out;
        st.store(waiter->WaitWithCallback(options, {}, &out));
        done.Down();
    };

    BOOST_REQUIRE_EQUAL(done.WaitTimeout(5000), 0);
    BOOST_CHECK(st.load() == WaitStatus::TimedOut);   // 旧通知没有被错误复用
}

/* ⑥ 登记失败不投递回调：RuntimeUnavailable、回调计数 0、输出为空 */
BOOST_AUTO_TEST_CASE(t_regist_fail_does_not_deliver)
{
    EnsureRuntime();
    auto waiter = CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int cb_count{0};
    std::atomic<WaitStatus> st{};
    std::atomic_bool has_value{true};

    bbtco [&]() {
        CoEventValue out = CoEventValue::From(std::int64_t{5});
        Coroutine::_TestFailNextAwaitRegist();
        st.store(waiter->WaitWithCallback(WaitOptions{}, [&]() {
            cb_count.fetch_add(1);
            return waiter->Notify(CoEventValue::From(std::int64_t{7})) == 0;
        }, &out));
        has_value.store(out.HasValue());
        done.Down();
    };

    BOOST_REQUIRE_EQUAL(done.WaitTimeout(5000), 0);
    BOOST_CHECK(st.load() == WaitStatus::RuntimeUnavailable);
    BOOST_CHECK_EQUAL(cb_count.load(), 0);
    BOOST_CHECK(!has_value.load());
}

/* ⑦ 晚到通知不污染后续等待：超时解绑后旧 Notify 失败，新等待用自己的载荷完成 */
BOOST_AUTO_TEST_CASE(t_late_notify_does_not_pollute_next_wait)
{
    EnsureRuntime();
    auto waiter = CoWaiter::Create();

    bbt::core::thread::CountDownLatch first_done{1};
    std::atomic<WaitStatus> st1{};
    bbtco [&]() {
        WaitOptions options;
        options.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{50};
        CoEventValue out;
        st1.store(waiter->WaitWithCallback(options, {}, &out));
        first_done.Down();
    };
    BOOST_REQUIRE_EQUAL(first_done.WaitTimeout(5000), 0);
    BOOST_CHECK(st1.load() == WaitStatus::TimedOut);

    BOOST_CHECK_EQUAL(waiter->Notify(CoEventValue::From(std::int64_t{9})), -1);

    bbt::core::thread::CountDownLatch done{1};
    std::atomic<WaitStatus> st2{};
    std::atomic<std::int64_t> got{0};
    bbtco [&]() {
        CoEventValue out;
        st2.store(waiter->WaitWithCallback(WaitOptions{}, [&]() {
            return waiter->Notify(CoEventValue::From(std::int64_t{11})) == 0;
        }, &out));
        std::int64_t v = 0;
        if (out.Get(v))
            got.store(v);
        done.Down();
    };
    BOOST_REQUIRE_EQUAL(done.WaitTimeout(5000), 0);
    BOOST_CHECK(st2.load() == WaitStatus::Completed);
    BOOST_CHECK_EQUAL(got.load(), 11);
}

/* ⑧ 协程级取消首胜：Cancelled 且不携带任何载荷 */
BOOST_AUTO_TEST_CASE(t_cancel_first_win_has_no_payload)
{
    EnsureRuntime();
    auto waiter = CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<WaitStatus> st{};
    std::atomic_bool has_value{true};
    std::atomic<Coroutine*> co_p{nullptr};

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        CoEventValue out = CoEventValue::From(std::int64_t{5});
        st.store(waiter->WaitWithCallback(WaitOptions{}, {}, &out));
        has_value.store(out.HasValue());
        done.Down();
    };

    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
    co_p.load()->RequestCancel();
    BOOST_REQUIRE_EQUAL(done.WaitTimeout(5000), 0);
    BOOST_CHECK(st.load() == WaitStatus::Cancelled);
    BOOST_CHECK(!has_value.load());
}

/* ⑨ 组合等待带载荷：fd/deadline/custom 首胜中 custom 携带载荷 */
BOOST_AUTO_TEST_CASE(t_combined_wait_delivers_payload)
{
    EnsureRuntime();
    auto waiter = CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<CombinedWaitStatus> st{};
    std::atomic<std::int64_t> got{0};

    bbtco [&]() {
        CoEventValue out;
        st.store(waiter->Wait(CombinedWaitOptions{}, [&]() {
            return waiter->Notify(CoEventValue::From(std::int64_t{42})) == 0;
        }, &out));
        std::int64_t v = 0;
        if (out.Get(v))
            got.store(v);
        done.Down();
    };

    BOOST_REQUIRE_EQUAL(done.WaitTimeout(5000), 0);
    BOOST_CHECK(st.load() == CombinedWaitStatus::Completed);
    BOOST_CHECK_EQUAL(got.load(), 42);
}

/* ⑩ 端到端错类型：同尺寸不同登记 ID 的类型读不回，输出不被改写 */
BOOST_AUTO_TEST_CASE(t_wrong_type_read_rejected_after_delivery)
{
    EnsureRuntime();
    auto waiter = CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_bool wrong_ok{false};
    std::atomic_bool out_untouched{false};

    bbtco [&]() {
        CoEventValue out;
        waiter->WaitWithCallback(WaitOptions{}, [&]() {
            return waiter->Notify(CoEventValue::From(std::int64_t{7})) == 0;
        }, &out);

        SameSizeOtherPayload other{555, 555};
        wrong_ok.store(out.Get(other));
        out_untouched.store(other.seq == 555 && other.code == 555u);
        done.Down();
    };

    BOOST_REQUIRE_EQUAL(done.WaitTimeout(5000), 0);
    BOOST_CHECK(!wrong_ok.load());
    BOOST_CHECK(out_untouched.load());
}

/* ⑪ 并发 Notify 首胜（多线程同时投递不同载荷，真实握手屏障）：
 *    恰一次成功、交付载荷与赢家一致、败方不覆盖；r1 指出原「首胜」用例只是
 *    同线程顺序两发，不能证明多线程竞争下的首胜与载荷归属。 */
BOOST_AUTO_TEST_CASE(t_concurrent_notify_single_winner_payload)
{
    EnsureRuntime();
    auto waiter = CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<WaitStatus> st{};
    std::atomic<std::int64_t> got{0};
    std::atomic<Coroutine*> co_p{nullptr};

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        CoEventValue out;
        st.store(waiter->WaitWithCallback(WaitOptions{}, {}, &out));
        std::int64_t v = 0;
        if (out.Get(v))
            got.store(v);
        done.Down();
    };

    /* 等待者真正进入挂起状态（登记完成）后再并发投递，不用 sleep 猜时序 */
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));

    constexpr int kThreads = 8;
    std::atomic_int ready{0};
    std::atomic_bool go{false};
    std::atomic_int ok{0}, failed{0};
    std::atomic<std::int64_t> winner{0};
    {
        std::vector<std::thread> ts;
        ts.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            ts.emplace_back([&, i]() {
                const std::int64_t payload = 100 + i;
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();   // 屏障：同时投递
                if (waiter->Notify(CoEventValue::From(payload)) == 0) {
                    ok.fetch_add(1);
                    winner.store(payload);
                } else {
                    failed.fetch_add(1);
                }
            });
        }
        while (ready.load(std::memory_order_acquire) != kThreads)
            std::this_thread::yield();
        go.store(true, std::memory_order_release);
        for (auto& t : ts)
            t.join();
    }

    BOOST_REQUIRE_EQUAL(done.WaitTimeout(5000), 0);
    BOOST_CHECK(st.load() == WaitStatus::Completed);
    BOOST_CHECK_EQUAL(ok.load(), 1);                  // 恰一次首胜
    BOOST_CHECK_EQUAL(failed.load(), kThreads - 1);
    BOOST_CHECK_EQUAL(winner.load(), got.load());     // 载荷与赢家一致，败方不覆盖
    BOOST_CHECK_NE(got.load(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
