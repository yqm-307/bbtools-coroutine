/**
 * @file Test_cowaiter_payload_rounds.cc
 * @brief 等待代次载荷隔离（#验收-1）：结果归属各轮事件/等待，
 *        上一轮已决议但等待者尚未恢复时，下一轮登记不得抹掉上一轮载荷。
 *
 * 为什么单列可执行：本用例需要确定性交错——用单 worker 让第二协程阻塞
 * worker，从而在「第一轮已决议、等待者未恢复」的窗口内完成第二轮登记。
 * 多 worker 下该窗口是真实竞态（时而命中时而错过），单列进程固定 1 worker
 * 才能把确定性交错变成生产回归。
 *
 * 覆盖两条同根因路径：普通窄接口带参等待 与 组合等待带参重载。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/sync/CoEventValue.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::sync;

BOOST_AUTO_TEST_SUITE(CoWaiterPayloadRoundTest)

namespace
{

/* 唯一一次 Start 前固定 1 worker：确定性交错的前提。 */
void EnsureRuntimeSingle()
{
    static std::once_flag once;
    std::call_once(once, [](){
        g_bbt_coroutine_config->m_cfg_static_thread_num = 1;
        bbt::coroutine::detail::Scheduler::GetInstance()->Start();
    });
}

bool Until(const std::function<bool()>& pred, int budget_ms = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

/* 借用受保护等待位做「本轮已登记」探针，不改变库语义。 */
struct ProbeWaiter : CoWaiter
{
    bool Ready()
    {
        std::lock_guard<std::mutex> l(m_notify_mutex);
        return m_co_event != nullptr;
    }
};

} // namespace

/* ⑪ 普通窄接口：第一轮载荷 7 必须在第二轮（载荷 11）登记后仍可读回 */
BOOST_AUTO_TEST_CASE(t_plain_wait_payload_round_isolated)
{
    EnsureRuntimeSingle();
    auto waiter = std::make_shared<ProbeWaiter>();

    std::atomic<bool> blocking{false}, release{false};
    std::atomic<int> done{0};
    std::atomic<std::int64_t> first{-1}, second{-1};
    std::atomic<bool> first_has{false}, second_has{false};

    bbtco [&]() {
        CoEventValue out;
        waiter->WaitWithCallback(WaitOptions{}, {}, &out);
        std::int64_t v = -1;
        first_has.store(out.Get(v));
        first.store(v);
        done.fetch_add(1);
    };
    BOOST_REQUIRE(Until([&]() { return waiter->Ready(); }));

    bbtco [&]() {
        blocking.store(true);
        if (!Until([&]() { return release.load(); })) { done.fetch_add(1); return; }
        CoEventValue out;
        waiter->WaitWithCallback(WaitOptions{},
            [&]() { return waiter->Notify(CoEventValue::From(std::int64_t{11})) == 0; },
            &out);
        std::int64_t v = -1;
        second_has.store(out.Get(v));
        second.store(v);
        done.fetch_add(1);
    };
    BOOST_REQUIRE(Until([&]() { return blocking.load(); }));

    BOOST_CHECK_EQUAL(waiter->Notify(CoEventValue::From(std::int64_t{7})), 0);
    release.store(true);
    BOOST_REQUIRE(Until([&]() { return done.load() == 2; }));

    BOOST_CHECK(first_has.load());
    BOOST_CHECK_EQUAL(first.load(), 7);
    BOOST_CHECK(second_has.load());
    BOOST_CHECK_EQUAL(second.load(), 11);
}

/* ⑫ 组合等待带参重载：同一根因，组合首胜载荷也不得被下一轮登记抹掉 */
BOOST_AUTO_TEST_CASE(t_combined_wait_payload_round_isolated)
{
    EnsureRuntimeSingle();
    auto waiter = std::make_shared<ProbeWaiter>();

    std::atomic<bool> blocking{false}, release{false};
    std::atomic<int> done{0};
    std::atomic<std::int64_t> first{-1}, second{-1};
    std::atomic<bool> first_has{false}, second_has{false};

    bbtco [&]() {
        CoEventValue out;
        waiter->Wait(CombinedWaitOptions{}, {}, &out);
        std::int64_t v = -1;
        first_has.store(out.Get(v));
        first.store(v);
        done.fetch_add(1);
    };
    BOOST_REQUIRE(Until([&]() { return waiter->Ready(); }));

    bbtco [&]() {
        blocking.store(true);
        if (!Until([&]() { return release.load(); })) { done.fetch_add(1); return; }
        CoEventValue out;
        waiter->Wait(CombinedWaitOptions{},
            [&]() { return waiter->Notify(CoEventValue::From(std::int64_t{11})) == 0; },
            &out);
        std::int64_t v = -1;
        second_has.store(out.Get(v));
        second.store(v);
        done.fetch_add(1);
    };
    BOOST_REQUIRE(Until([&]() { return blocking.load(); }));

    BOOST_CHECK_EQUAL(waiter->Notify(CoEventValue::From(std::int64_t{7})), 0);
    release.store(true);
    BOOST_REQUIRE(Until([&]() { return done.load() == 2; }));

    BOOST_CHECK(first_has.load());
    BOOST_CHECK_EQUAL(first.load(), 7);
    BOOST_CHECK(second_has.load());
    BOOST_CHECK_EQUAL(second.load(), 11);
}

BOOST_AUTO_TEST_SUITE_END()
