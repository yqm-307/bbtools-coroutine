#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/CfsRunQueue.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;
using namespace std::chrono_literals;

namespace {
void Put(CfsRunQueue& queue, CfsEntity& entity, uint64_t weight = 1)
{
    BOOST_REQUIRE(queue.Enqueue(entity, nullptr, CO_PRIORITY_NORMAL, weight));
}
void Run(CfsRunQueue& queue, CfsEntity& entity, uint64_t service, bool waiting = false)
{
    BOOST_REQUIRE(queue.Pick() == &entity);
    queue.Account(entity, service, waiting);
}
bool Ready(std::future<void>& future)
{
    return future.wait_for(3s) == std::future_status::ready;
}
template<class F> bool Until(F predicate)
{
    const auto limit = std::chrono::steady_clock::now() + 3s;
    while (!predicate() && std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(1ms);
    return predicate();
}
}

BOOST_AUTO_TEST_CASE(algorithm_migration_global_debt)
{
    CfsEntity warm_source, warm_target, debtor, anchor, fresh;
    CfsRunQueue source, target;
    Put(source, warm_source); Run(source, warm_source, 100);
    Put(target, warm_target); Run(target, warm_target, 1000);
    Put(source, debtor); Put(source, anchor);
    Run(source, debtor, 80);
    Put(source, debtor);
    BOOST_REQUIRE(source.Take() == &anchor);
    // 与 global 一样：只传递节点，不携带源队列/P 引用，不重置账目。
    moodycamel::ConcurrentQueue<CfsEntity*> transit;
    BOOST_REQUIRE(transit.enqueue(source.Take()));
    CfsEntity* moved = nullptr;
    BOOST_REQUIRE(transit.try_dequeue(moved));
    BOOST_REQUIRE(moved == &debtor);
    Put(target, *moved); Put(target, fresh);
    Run(target, fresh, 40); Put(target, fresh);
    Run(target, fresh, 40); Put(target, fresh);
    // 债务必须是80：清零/直接比较旧绝对值都会让 debtor 提前当选。
    Run(target, debtor, 1);
    BOOST_CHECK_EQUAL(debtor.ServiceUs(), 81u);
    BOOST_CHECK(target.Take() == &fresh);
}

BOOST_AUTO_TEST_CASE(algorithm_stable_wake_account_and_weight)
{
    CfsEntity running, incoming, sleeper, later;
    CfsRunQueue queue;
    Put(queue, sleeper); Run(queue, sleeper, 10, true);
    Put(queue, running);
    BOOST_REQUIRE(queue.Pick() == &running);
    // New/Wake 可在 Resume 期间进入，但不能覆盖当前运行者的 Account。
    Put(queue, incoming); Put(queue, sleeper);
    queue.Account(running, 100, false);
    Put(queue, running);
    Run(queue, incoming, 10); // 相同键稳定
    Run(queue, sleeper, 10, true);
    Put(queue, later);
    Put(queue, sleeper);
    Run(queue, later, 1); // wake 未保留长期历史信用抢在先入队的新任务前
    Run(queue, sleeper, 1);
    BOOST_CHECK_EQUAL(sleeper.ServiceUs(), 21u);
    BOOST_CHECK(queue.Take() == &running);

    CfsEntity heavy, light;
    CfsRunQueue weighted;
    Put(weighted, heavy, 4); Put(weighted, light, 1);
    Run(weighted, heavy, 8); Put(weighted, heavy, 4);
    Run(weighted, light, 8); Put(weighted, light, 1);
    Run(weighted, heavy, 0); // 零经过时间也正记账
    BOOST_CHECK_EQUAL(heavy.ServiceUs(), 9u);
    BOOST_CHECK(weighted.Take() == &light);
}

BOOST_AUTO_TEST_CASE(algorithm_wait_preserves_debt_without_old_credit)
{
    CfsEntity sleeper, anchor, fresh;
    CfsRunQueue queue;
    Put(queue, sleeper); Put(queue, anchor);
    Run(queue, sleeper, 100, true);
    Run(queue, anchor, 1000); // 等待期间本地 floor 已显著推进
    Put(queue, sleeper); Put(queue, fresh);
    Run(queue, fresh, 50); Put(queue, fresh);
    Run(queue, fresh, 50); Put(queue, fresh);
    Run(queue, sleeper, 1);
    BOOST_CHECK_EQUAL(sleeper.ServiceUs(), 101u);
    BOOST_CHECK(queue.Take() == &fresh);
}

BOOST_AUTO_TEST_CASE(single_p_external_progress_before_hot_release)
{
    g_bbt_coroutine_config->m_cfg_static_thread_num = 1;
    g_bbt_coroutine_config->m_cfg_stack_size = 65536;
    g_scheduler->Start();
    std::atomic_bool release{false};
    std::atomic<int> turns{0}, hot_done{0};
    sync::CoWaiter waiter;
    std::promise<void> registered, resumed, submitted;
    auto registration = registered.get_future();
    auto wake_done = resumed.get_future();
    auto submit_done = submitted.get_future();
    g_scheduler->RegistCoroutineTask([&] {
        sync::CombinedWaitOptions options;
        waiter.Wait(options, [&] { registered.set_value(); return true; });
        resumed.set_value();
    });
    BOOST_REQUIRE(Ready(registration));
    for (int n = 0; n < 4; ++n)
        g_scheduler->RegistCoroutineTask([&] {
            while (!release.load()) { ++turns; bbtco_yield; }
            ++hot_done;
        });
    BOOST_REQUIRE(Until([&] { return turns.load() >= 100; }));
    const auto before = turns.load();
    // 主线程是外部生产者；热点结束门保持关闭直到两条来源都获得执行。
    g_scheduler->RegistCoroutineTask([&] { submitted.set_value(); });
    BOOST_CHECK_EQUAL(waiter.Notify(CoEventValue::From(true)), 0);
    const bool submit_progress = Ready(submit_done);
    const bool wake_progress = Ready(wake_done);
    const bool hot_live = hot_done.load() == 0 && !release.load();
    release = true;
    BOOST_CHECK(submit_progress);
    BOOST_CHECK(wake_progress);
    BOOST_CHECK(hot_live);
    BOOST_CHECK_GE(turns.load(), before);
    BOOST_REQUIRE(Until([&] { return hot_done.load() == 4; }));
}

BOOST_AUTO_TEST_CASE(multi_p_steal_preserves_running_progress)
{
    auto* cfg = g_bbt_coroutine_config.get();
    cfg->m_cfg_static_thread_num = 2;
    cfg->m_cfg_stack_size = 65536;
    g_scheduler->Start();
    auto ids = g_scheduler->GetWorkerProcesserIds();
    std::promise<void> hold_other, other_entered, other_done;
    auto other_gate = hold_other.get_future();
    auto entered = other_entered.get_future();
    auto finished = other_done.get_future();
    std::atomic<ProcesserId> occupied{0};
    g_scheduler->RegistCoroutineTaskOn(ids[1], [&] {
        occupied = Processer::GetLocalProcesser()->GetId();
        other_entered.set_value(); other_gate.wait(); other_done.set_value();
    });
    BOOST_REQUIRE(Ready(entered));
    const auto source = occupied.load() == ids[0] ? ids[1] : ids[0];
    std::atomic_bool hot_release{false}, block_now{false};
    std::atomic<int> original_turns{0}, migrated_turns{0};
    std::promise<void> hot_done, block_entered, block_release, block_done;
    auto hot_finished = hot_done.get_future();
    auto blocked = block_entered.get_future();
    auto block_gate = block_release.get_future();
    auto block_finished = block_done.get_future();
    g_scheduler->RegistCoroutineTaskOn(source, [&] {
        while (!hot_release.load()) {
            if (Processer::GetLocalProcesser()->GetId() == source) ++original_turns;
            else ++migrated_turns;
            bbtco_yield;
        }
        hot_done.set_value();
    });
    BOOST_REQUIRE(Until([&] { return original_turns.load() >= 100; }));
    g_scheduler->RegistCoroutineTaskOn(source, [&] {
        block_now = true; block_entered.set_value(); block_gate.wait(); block_done.set_value();
    });
    BOOST_REQUIRE(Ready(blocked));
    // source 正执行不 yield 的任务，且持有已运行过的 READY 热点积压。
    hold_other.set_value();
    const bool migrated = Until([&] { return migrated_turns.load() >= 100; });
    BOOST_CHECK(block_now.load());
    BOOST_CHECK(migrated); // 在释放 source 结束门前，另一 P 已偷到并持续执行
    hot_release = true;
    block_release.set_value();
    BOOST_REQUIRE(Ready(hot_finished));
    BOOST_REQUIRE(Ready(block_finished));
    BOOST_REQUIRE(Ready(finished));
}
