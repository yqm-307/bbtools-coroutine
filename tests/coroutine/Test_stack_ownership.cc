/**
 * @file Test_stack_ownership.cc
 * @brief 精确预算、异步交付与取消交错；测试框架只在 native 控制线程执行。
 */
#define BOOST_TEST_MODULE stack_ownership
#include <boost/test/included/unit_test.hpp>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Context.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;
namespace {
constexpr size_t kSmall = 12 * 1024;
constexpr size_t kLarge = 128 * 1024;
static_assert(!std::is_copy_constructible_v<StackLease>);
static_assert(std::is_nothrow_move_constructible_v<StackLease>);

struct ConfigMaxGuard {
    size_t& value;
    size_t original;

    ~ConfigMaxGuard() { value = original; }
};

template <class Predicate> bool Await(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

void Empty(StackPool& pool)
{
    const auto s = pool.Observe();
    BOOST_CHECK_EQUAL(s.borrowed, 0u);
    BOOST_CHECK_EQUAL(s.pending, 0u);
    BOOST_CHECK_EQUAL(s.pending_usable_bytes, 0u);
    BOOST_CHECK_EQUAL(s.live, s.idle);
    pool.ReleaseUnused();
    const auto final = pool.Observe();
    BOOST_CHECK_EQUAL(final.live, 0u);
    BOOST_CHECK_EQUAL(final.committed_usable_bytes, 0u);
}

void StartRuntime()
{
    g_bbt_coroutine_config->m_cfg_static_thread_num = 1;
    g_bbt_stackpoll->Configure({StackPolicy::Auto, {}, 16, 4 * 1024 * 1024}, {64 * 1024, true});
    g_scheduler->Start();
}

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic_bool entered{false};
    bool open{false};
    void Block()
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered.store(true);
        cv.wait(lock, [this] { return open; });
    }
    void Open()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
    ~Gate() { Open(); }
};
Gate* g_gate = nullptr;
void BlockAllocation()
{
    g_gate->Block();
}

/**
 * 阶段由维护流 hook 握手，不靠 sleep 猜测。probe 只让出 CPU，不借助被阻塞的 poller。
 */
void Race(bool after_commit, bool cancel, bool timeout)
{
    StartRuntime();
    Gate gate;
    g_gate = &gate;
    std::atomic<Coroutine*> owner{nullptr};
    std::atomic_bool parent_done{false}, probe_done{false}, progressed{false};
    std::atomic_int calls{0}, outcome{0};
    const auto deadline = timeout ? std::chrono::steady_clock::now() + std::chrono::milliseconds(150) : Deadline::max();
    g_scheduler->RegistCoroutineTask(64 * 1024, [&] {
        while (!gate.entered.load()) {
            bbtco_yield;
        }
        progressed.store(true);
        while (!parent_done.load()) {
            bbtco_yield;
        }
        probe_done.store(true);
    });
    g_scheduler->RegistCoroutineTask(64 * 1024, [&] {
        owner.store(Processer::TryGetLocalProcesser()->GetCurrentCoroutine());
        if (after_commit)
            StackPool::_TestAfterCommit(BlockAllocation);
        else
            StackPool::_TestBeforeAllocation(BlockAllocation);
        try {
            g_scheduler->RegistCoroutineTask(
                StackRequest{kLarge, true},
                [&] {
                    if (GetLocalCoroutineStackSize() == kLarge)
                        ++calls;
                },
                WaitOptions{deadline});
            outcome.store(1);
        } catch (const StackWaitError& e) {
            outcome.store(e.Status() == WaitStatus::Cancelled ? 2 : e.Status() == WaitStatus::TimedOut ? 3 : 4);
        } catch (...) {
            outcome.store(5);
        }
        parent_done.store(true);
    });
    BOOST_REQUIRE(Await([&] { return gate.entered.load(); }));
    BOOST_REQUIRE(Await([&] { return progressed.load(); }));
    const auto held = g_bbt_stackpoll->Observe();
    BOOST_CHECK_EQUAL(calls.load(), 0);
    BOOST_CHECK_EQUAL(held.pending, after_commit ? 0u : 1u);
    BOOST_CHECK_EQUAL(held.pending_usable_bytes, after_commit ? 0u : kLarge);
    BOOST_CHECK_EQUAL(held.borrowed, after_commit ? 3u : 2u);
    BOOST_CHECK_EQUAL(held.committed_usable_bytes, 2 * 64 * 1024 + (after_commit ? kLarge : 0));
    if (cancel)
        owner.load()->RequestCancel();
    if (timeout)
        BOOST_REQUIRE(Await([&] { return std::chrono::steady_clock::now() >= deadline; }));
    gate.Open();
    BOOST_REQUIRE(Await([&] { return parent_done.load() && probe_done.load(); }));
    BOOST_REQUIRE(
        Await([&] { return g_bbt_stackpoll->Observe().borrowed == 0 && g_bbt_stackpoll->Observe().pending == 0; }));
    BOOST_CHECK_EQUAL(outcome.load(), cancel ? 2 : timeout ? 3 : 1);
    BOOST_CHECK_EQUAL(calls.load(), cancel || timeout ? 0 : 1);
    Empty(*g_bbt_stackpoll);
    g_gate = nullptr;
}
}

BOOST_AUTO_TEST_SUITE(stack_ownership)

BOOST_AUTO_TEST_CASE(exact_buckets)
{
    StackPool pool;
    StackConfig config{StackPolicy::Strict,
                       {{{kSmall, true}, 1}, {{kSmall, false}, 1}, {{kLarge, true}, 1}, {{kLarge, false}, 1}},
                       0,
                       0};
    pool.Configure(config, {kSmall, true});
    std::vector<StackLease> leases;
    std::vector<Stack*> addresses;
    for (const auto& limit : config.strict_classes) {
        auto lease = pool.Acquire(limit.request);
        BOOST_REQUIRE(lease);
        BOOST_CHECK_EQUAL(lease->UseableSize(), limit.request.usable_bytes);
        BOOST_CHECK_EQUAL(lease->IsProtected(), limit.request.protect);
        lease->StackBottom()[0] = 41;
        addresses.push_back(lease.get());
        leases.push_back(std::move(lease));
    }
    BOOST_CHECK(!pool.Acquire({kSmall, true}));
    BOOST_CHECK(!pool.Acquire({32 * 1024, true}));
    BOOST_CHECK_EQUAL(pool.ReleaseUnused(), 0u);
    pool.OnUpdate();
    auto held = pool.Observe();
    BOOST_CHECK_EQUAL(held.borrowed, 4u);
    BOOST_CHECK_EQUAL(held.committed_usable_bytes, 2 * (kSmall + kLarge));
    for (size_t i = 0; i != leases.size(); ++i) {
        BOOST_CHECK_EQUAL(leases[i].get(), addresses[i]);
        BOOST_CHECK_EQUAL(leases[i]->StackBottom()[0], 41);
        const auto request = config.strict_classes[i].request;
        leases[i].reset();
        auto reused = pool.Acquire(request);
        BOOST_REQUIRE(reused);
        BOOST_CHECK_EQUAL(reused.get(), addresses[i]);
        BOOST_CHECK_EQUAL(reused->UseableSize(), request.usable_bytes);
        BOOST_CHECK_EQUAL(reused->IsProtected(), request.protect);
        leases[i] = std::move(reused);
    }
    leases.clear();
    BOOST_CHECK(pool.CanAdmit(config.strict_classes[0].request, 1));
    auto reused = pool.Acquire(config.strict_classes[0].request);
    BOOST_REQUIRE(reused);
    reused.reset();
    Empty(pool);
}

BOOST_AUTO_TEST_CASE(auto_budget)
{
    StackPool pool;
    pool.Configure({StackPolicy::Auto, {}, 3, kSmall + kLarge}, {kSmall, true});
    auto small = pool.Acquire({kSmall, true});
    auto large = pool.Acquire({kLarge, false});
    BOOST_REQUIRE(small && large);
    auto* address = large->StackTop();
    large->StackBottom()[0] = 63;

    /**
     * 对象尚有余量，字节已满。
     */
    BOOST_CHECK(!pool.Acquire({kSmall, false}));
    small.reset();

    /**
     * 只回收 idle，不搬动 borrowed。
     */
    auto other = pool.Acquire({kSmall, false});
    BOOST_REQUIRE(other);
    BOOST_CHECK(!other->IsProtected());
    BOOST_CHECK_EQUAL(large->StackTop(), address);
    BOOST_CHECK_EQUAL(large->StackBottom()[0], 63);
    BOOST_CHECK_EQUAL(pool.Observe().committed_usable_bytes, kSmall + kLarge);
    other.reset();
    large.reset();
    Empty(pool);

    pool.Configure({StackPolicy::Auto, {}, 2, kSmall + kLarge}, {kSmall, true});
    auto cached = pool.Acquire({kSmall, true});
    BOOST_REQUIRE(cached);
    cached.reset();
    pool.Configure({StackPolicy::Auto, {}, 2, kSmall}, {kSmall, true});
    auto cached_budget = pool.Acquire({kSmall, true});
    BOOST_REQUIRE(cached_budget);
    cached_budget.reset();
    BOOST_CHECK(!pool.CanAdmit({kSmall, true}, 2));
    Empty(pool);

    pool.Configure({StackPolicy::Auto, {}, 2, kSmall + kLarge}, {kSmall, true});
    auto held = pool.Acquire({kLarge, true});
    BOOST_REQUIRE(held);
    BOOST_CHECK(pool.CanAdmit({kSmall, false}, 1));
    auto cross_bucket = pool.Acquire({kSmall, false});
    BOOST_REQUIRE(cross_bucket);
    cross_bucket.reset();
    held.reset();
    Empty(pool);

    pool.Configure({StackPolicy::Auto, {}, 1, 1024 * 1024}, {kSmall, true});
    small = pool.Acquire({kSmall, true});

    /**
     * 字节有余量，对象已满。
     */
    BOOST_CHECK(!pool.Acquire({kSmall, false}));
    small.reset();
    Empty(pool);

    ConfigMaxGuard restore_max{g_bbt_coroutine_config->m_cfg_max_coroutine,
                               g_bbt_coroutine_config->m_cfg_max_coroutine};
    g_bbt_coroutine_config->m_cfg_max_coroutine = 1;
    {
        StackPool legacy;
        auto cached_legacy = legacy.Acquire({kSmall, true});
        BOOST_REQUIRE(cached_legacy);
        cached_legacy.reset();
        auto held_legacy = legacy.Acquire({kSmall, false});
        BOOST_REQUIRE(held_legacy);
        BOOST_CHECK(!legacy.CanAdmit({kSmall, true}, 1));
        BOOST_CHECK(!legacy.Acquire({kSmall, true}));
        held_legacy.reset();
        Empty(legacy);
    }
}

BOOST_AUTO_TEST_CASE(config_validation)
{
    const StackRequest request{kSmall, true};
    const auto invalid = [&](StackConfig config) {
        BOOST_CHECK_THROW(StackPool::ValidateConfig(config, request), std::exception);
    };
    invalid({StackPolicy::Strict, {}, 0, 0});
    invalid({StackPolicy::Strict, {{request, 0}}, 0, 0});
    invalid({StackPolicy::Strict, {{request, 1}, {{kSmall - 1, true}, 2}}, 0, 0});
    invalid({StackPolicy::Strict, {{request, 1}}, 1, 0});
    invalid({StackPolicy::Strict, {{{kLarge, true}, 1}}, 0, 0});
    invalid({StackPolicy::Strict, {{request, std::numeric_limits<size_t>::max()}}, 0, 0});
    invalid({StackPolicy::Auto, {}, 0, kLarge});
    invalid({StackPolicy::Auto, {}, 2, kSmall - 1});
    invalid({StackPolicy::Auto, {{request, 1}}, 2, kLarge});
    BOOST_CHECK_NO_THROW(StackPool::ValidateConfig({StackPolicy::Strict, {{request, 1}}, 0, 0}, request));
    BOOST_CHECK_NO_THROW(StackPool::ValidateConfig({StackPolicy::Auto, {}, 1, kSmall}, request));
}

BOOST_AUTO_TEST_CASE(native_failures)
{
    StackPool pool;
    pool.Configure({StackPolicy::Auto, {}, 2, 2 * kLarge}, {kSmall, true});
    Stack::_TestFailNextAlloc();
    BOOST_CHECK_THROW(pool.Acquire({kLarge, true}), std::bad_alloc);
    Empty(pool);
    Stack::_TestFailNextApplyProtect();
    BOOST_CHECK_THROW(pool.Acquire({kLarge, true}), std::system_error);
    Empty(pool);
    auto lease = pool.Acquire({kLarge, true});
    StackPool::_TestFailNextPoolPush();
    lease.reset();
    BOOST_CHECK_EQUAL(pool.Observe().live, 0u);
    Empty(pool);
    auto context_lease = pool.Acquire({kLarge, true});
    Context::_TestFailNextCreate();
    BOOST_CHECK_THROW(Context(std::move(context_lease), [] {}), std::runtime_error);
    BOOST_CHECK(!context_lease);
    Empty(pool);
    BOOST_CHECK_THROW(pool.Acquire({kLarge, true}, {std::chrono::steady_clock::now()}), StackWaitError);
    Empty(pool);
}

BOOST_AUTO_TEST_CASE(async_failures)
{
    StartRuntime();

    /**
     * 每种故障都走真实 Scheduler→Acquire→Context→入队链；不运行半初始化 child。
     */
    for (int fault = 0; fault != 6; ++fault) {
        std::atomic_bool done{false};
        std::atomic_int outcome{0}, calls{0};
        g_scheduler->RegistCoroutineTask(64 * 1024, [&, fault] {
            if (fault == 0)
                Stack::_TestFailNextAlloc();
            if (fault == 1)
                Stack::_TestFailNextApplyProtect();
            if (fault == 2)
                StackPool::_TestFailNextPost();
            if (fault == 3)
                Context::_TestFailNextCreate();
            if (fault == 4)
                Scheduler::_TestFailNextSubmit();
            if (fault == 5)
                Coroutine::_TestFailNextAwaitRegist();
            try {
                g_scheduler->RegistCoroutineTask(StackRequest{kLarge, true}, [&] { ++calls; });
                outcome.store(9);
            } catch (const StackWaitError& e) {
                outcome.store(e.Status() == WaitStatus::RuntimeUnavailable ? 4 : 8);
            } catch (const std::bad_alloc&) {
                outcome.store(1);
            } catch (const std::system_error&) {
                outcome.store(2);
            } catch (const std::runtime_error&) {
                outcome.store(3);
            } catch (...) {
                outcome.store(8);
            }
            done.store(true);
        });
        BOOST_REQUIRE(Await([&] { return done.load(); }));
        BOOST_REQUIRE(
            Await([&] { return g_bbt_stackpoll->Observe().borrowed == 0 && g_bbt_stackpoll->Observe().pending == 0; }));
        const int expected[] = {1, 2, 1, 3, 3, 4};
        BOOST_CHECK_EQUAL(outcome.load(), expected[fault]);
        BOOST_CHECK_EQUAL(calls.load(), 0);
        Empty(*g_bbt_stackpoll);
    }
}

BOOST_AUTO_TEST_CASE(pending_cancel)
{
    Race(false, true, false);
}
BOOST_AUTO_TEST_CASE(committed_cancel)
{
    Race(true, true, false);
}
BOOST_AUTO_TEST_CASE(pending_deadline)
{
    Race(false, false, true);
}
BOOST_AUTO_TEST_CASE(single_worker_progress)
{
    Race(false, false, false);
}

BOOST_AUTO_TEST_SUITE_END()
