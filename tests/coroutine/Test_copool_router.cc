/** @file Test_copool_router.cc
 * @brief 尺寸路由池：精确类路由、有界队列/并发、关闭竞争、异常隔离与自等待拒绝。
 *
 * 每个用例在独立进程中运行，runtime 只启动一次且只有 1 个 Processer：
 * 路由与关闭不只靠多线程才能推进，这条约束由整组用例共同证明。
 */
#define BOOST_TEST_MODULE copool_router
#include <boost/test/included/unit_test.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <vector>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Stack.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>

using namespace bbt::coroutine;

namespace {

constexpr size_t kSmall = 12 * 1024;
constexpr size_t kMedium = 64 * 1024;
constexpr size_t kLarge = 128 * 1024;
constexpr int kBudgetMs = 5000;

/** 单 Processer 进程寿命 runtime；本文件每个用例独立进程，只会启动一次。 */
void StartRuntime() {
    g_bbt_coroutine_config->m_cfg_static_thread_num = 1;
    g_scheduler->Start();
}

template <class Predicate>
bool Await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kBudgetMs);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

pool::CoPoolOptions Options(std::initializer_list<pool::CoPoolClassConfig> classes) {
    pool::CoPoolOptions options;
    options.classes.assign(classes.begin(), classes.end());
    return options;
}

/** 观察每个类的 worker 真实栈尺寸：任务跑在 worker 栈上，读到的就是类的规格。 */
struct ClassProbe {
    std::atomic_int runs{0};
    std::atomic_size_t last_stack{0};
};

void ExpectStack(const ClassProbe& probe, size_t expected) {
    BOOST_CHECK_EQUAL(probe.runs.load(), 3);
    BOOST_CHECK_EQUAL(probe.last_stack.load(), expected);
}

} // namespace

BOOST_AUTO_TEST_SUITE(copool_router)

/** 三类栈按 (U, protect) 精确路由：不跨桶、不降规格、不接受未声明保护属性。 */
BOOST_AUTO_TEST_CASE(mixed_size) {
    StartRuntime();
    ClassProbe small, medium, large;
    auto pool = pool::CoPool::Create(Options({
        {{kSmall, true}, 1, 4},
        {{kMedium, false}, 1, 4},
        {{kLarge, true}, 1, 4},
    }));

    for (int i = 0; i < 3; ++i) {
        BOOST_CHECK(pool->Submit({kSmall, true}, [&small] {
            small.last_stack.store(GetLocalCoroutineStackSize());
            small.runs.fetch_add(1);
        }) == pool::CoPoolSubmitStatus::Accepted);
        BOOST_CHECK(pool->Submit({kMedium, false}, [&medium] {
            medium.last_stack.store(GetLocalCoroutineStackSize());
            medium.runs.fetch_add(1);
        }) == pool::CoPoolSubmitStatus::Accepted);
        BOOST_CHECK(pool->Submit({kLarge, true}, [&large] {
            large.last_stack.store(GetLocalCoroutineStackSize());
            large.runs.fetch_add(1);
        }) == pool::CoPoolSubmitStatus::Accepted);
    }

    // 未声明规格：尺寸或保护属性不精确匹配都拒绝，而不是退回默认类。
    BOOST_CHECK(pool->Submit({kSmall + 1, true}, [] {}) == pool::CoPoolSubmitStatus::NoExactClass);
    BOOST_CHECK(pool->Submit({kMedium, true}, [] {}) == pool::CoPoolSubmitStatus::NoExactClass);
    BOOST_CHECK(pool->Submit({0, true}, [] {}) == pool::CoPoolSubmitStatus::NoExactClass);
    BOOST_CHECK_EQUAL(pool->Submit([] {}), -1);          // 尺寸路由池无默认类
    BOOST_CHECK(!pool->SubmitWithFuture([] {}).valid()); // future 提交同样要显式尺寸

    BOOST_REQUIRE(Await([&] { return small.runs.load() == 3 && medium.runs.load() == 3 &&
                                     large.runs.load() == 3; }));
    ExpectStack(small, kSmall);
    ExpectStack(medium, kMedium);
    ExpectStack(large, kLarge);
    pool->Release();
}

/** 队列容量与每类 worker 数都有界：超额立即拒绝，排队任务不并发执行。 */
BOOST_AUTO_TEST_CASE(queue_full) {
    StartRuntime();
    auto pool = pool::CoPool::Create(Options({{{kSmall, true}, 1, 2}}));

    std::atomic_bool running{false}, second_started{false}, open{false};
    std::atomic_int done{0};

    BOOST_CHECK(pool->Submit({kSmall, true}, [&] {
        running.store(true);
        while (!open.load())
            bbtco_sleep(1); // 占住唯一 worker，后续任务只能排队或被拒
        done.fetch_add(1);
    }) == pool::CoPoolSubmitStatus::Accepted);
    BOOST_REQUIRE(Await([&] { return running.load(); }));

    BOOST_CHECK(pool->Submit({kSmall, true}, [&] { done.fetch_add(1); }) ==
                pool::CoPoolSubmitStatus::Accepted);
    BOOST_CHECK(pool->Submit({kSmall, true}, [&] {
        second_started.store(true);
        done.fetch_add(1);
    }) == pool::CoPoolSubmitStatus::Accepted);
    // 容量 2 已满：不再接纳，也不等待空间。
    BOOST_CHECK(pool->Submit({kSmall, true}, [] {}) == pool::CoPoolSubmitStatus::QueueFull);

    open.store(true);
    BOOST_REQUIRE(Await([&] { return done.load() == 3; }));
    BOOST_CHECK(second_started.load());
    pool->Release();
}

/** Release 与并发 Submit 竞争：封口后的提交明确拒绝，已入队任务不再执行。 */
BOOST_AUTO_TEST_CASE(release_submit_race) {
    StartRuntime();
    auto pool = pool::CoPool::Create(Options({{{kMedium, true}, 1, 8}}));

    std::atomic_int executed{0}, accepted{0}, rejected{0};
    std::atomic_bool stop{false}, saw_released{false};
    std::thread producer([&] {
        while (!stop.load()) {
            const auto status = pool->Submit({kMedium, true}, [&] { executed.fetch_add(1); });
            if (status == pool::CoPoolSubmitStatus::Accepted)
                accepted.fetch_add(1);
            else
                rejected.fetch_add(1);
            if (status == pool::CoPoolSubmitStatus::Released)
                saw_released.store(true);
        }
    });

    BOOST_REQUIRE(Await([&] { return executed.load() > 0; }));
    pool->Release();
    stop.store(true);
    producer.join();

    BOOST_CHECK_GT(accepted.load(), 0);
    BOOST_CHECK_GT(rejected.load(), 0);
    BOOST_CHECK(saw_released.load());
    BOOST_CHECK(pool->Submit({kMedium, true}, [] {}) == pool::CoPoolSubmitStatus::Released);

    // Release 返回即代表 worker 已退出、队列已排空：之后不能再有任务体执行。
    const int settled = executed.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    BOOST_CHECK_EQUAL(executed.load(), settled);
}

/** 任务异常被池隔离：计数可见、正常任务全部完成、池继续接纳。 */
BOOST_AUTO_TEST_CASE(exception_isolation) {
    StartRuntime();
    auto pool = pool::CoPool::Create(Options({{{kSmall, true}, 2, 32}}));

    std::atomic_int ok{0};
    for (int i = 0; i < 30; ++i) {
        if (i % 2 == 0) {
            BOOST_CHECK(pool->Submit({kSmall, true}, [] { throw std::runtime_error("copool task"); }) ==
                        pool::CoPoolSubmitStatus::Accepted);
        } else {
            BOOST_CHECK(pool->Submit({kSmall, true}, [&ok] { ok.fetch_add(1); }) ==
                        pool::CoPoolSubmitStatus::Accepted);
        }
    }
    BOOST_REQUIRE(Await([&] { return ok.load() == 15; }));
    BOOST_REQUIRE(Await([&] { return pool->GetUnhandledExceptionCount() == 15u; }));

    BOOST_CHECK(pool->Submit({kSmall, true}, [&ok] { ok.fetch_add(1); }) ==
                pool::CoPoolSubmitStatus::Accepted);
    BOOST_REQUIRE(Await([&] { return ok.load() == 16; }));
    pool->Release();
}

/** 池自身 worker 内关闭是自等待：抛出 logic_error 且不改变池状态。 */
BOOST_AUTO_TEST_CASE(same_pool_dependency) {
    StartRuntime();
    auto pool = pool::CoPool::Create(Options({{{kSmall, true}, 1, 2}}));

    std::atomic_bool rejected{false}, done{false};
    BOOST_CHECK(pool->Submit({kSmall, true}, [&] {
        try {
            pool->Release();
        } catch (const std::logic_error&) {
            rejected.store(true);
        }
        done.store(true);
    }) == pool::CoPoolSubmitStatus::Accepted);
    BOOST_REQUIRE(Await([&] { return done.load(); }));
    BOOST_CHECK(rejected.load());

    // 自等待拒绝没有任何关闭副作用：池仍接纳并执行任务。
    std::atomic_bool ran{false};
    BOOST_CHECK(pool->Submit({kSmall, true}, [&ran] { ran.store(true); }) ==
                pool::CoPoolSubmitStatus::Accepted);
    BOOST_REQUIRE(Await([&] { return ran.load(); }));
    pool->Release();
}

/** 创建前的容量/参数校验：Strict 未声明类、非法尺寸类、非法选项都在建 worker 前失败。 */
BOOST_AUTO_TEST_CASE(empty_class) {
    g_bbt_stackpoll->Configure(StackConfig{StackPolicy::Strict, {{{kSmall, true}, 4}}, 0, 0},
                               StackRequest{kSmall, true});
    StartRuntime();

    const auto before = g_bbt_stackpoll->Observe();
    // Strict 只声明了 12 KiB/protected：其它尺寸或保护属性不扩预算、不跨桶。
    BOOST_CHECK_THROW(pool::CoPool::Create(Options({{{kLarge, true}, 1, 1}})), std::runtime_error);
    BOOST_CHECK_THROW(pool::CoPool::Create(Options({{{kSmall, false}, 1, 1}})), std::runtime_error);
    // 预算是硬上限：2 个 worker 超出 max_objects=4 也不算可接纳。
    BOOST_CHECK_THROW(pool::CoPool::Create(Options({{{kSmall, true}, 5, 1}})), std::runtime_error);
    const auto after = g_bbt_stackpoll->Observe();
    BOOST_CHECK_EQUAL(after.live, before.live);
    BOOST_CHECK_EQUAL(after.borrowed, 0u);
    BOOST_CHECK_EQUAL(after.pending, 0u);

    // 非法选项在任何 worker 创建前被拒绝。
    BOOST_CHECK_THROW(pool::CoPool::Create(pool::CoPoolOptions{}), std::invalid_argument);
    BOOST_CHECK_THROW(pool::CoPool::Create(Options({{{kSmall, true}, 0, 1}})), std::invalid_argument);
    BOOST_CHECK_THROW(pool::CoPool::Create(Options({{{kSmall, true}, 1, 0}})), std::invalid_argument);
    BOOST_CHECK_THROW(pool::CoPool::Create(Options({{{0, true}, 1, 1}})), std::invalid_argument);
    BOOST_CHECK_THROW(pool::CoPool::Create(Options({{{kSmall, true}, 1, 1}, {{kSmall, true}, 1, 1}})),
                      std::invalid_argument);

    auto pool = pool::CoPool::Create(Options({{{kSmall, true}, 1, 1}}));
    BOOST_CHECK(pool->Submit({kLarge, true}, [] {}) == pool::CoPoolSubmitStatus::NoExactClass);
    std::atomic_bool ran{false};
    BOOST_CHECK(pool->Submit({kSmall, true}, [&ran] { ran.store(true); }) ==
                pool::CoPoolSubmitStatus::Accepted);
    BOOST_REQUIRE(Await([&] { return ran.load(); }));
    pool->Release();
}

/** worker 栈分配失败：构造回滚已建 worker，预算计数回到创建前。 */
BOOST_AUTO_TEST_CASE(allocation_failure) {
    StartRuntime();

    // 先让缓存有一份 128 KiB 栈，保证故障落在第二个 worker 的系统分配上，
    // 这样才覆盖「部分 worker 已登记 → 回滚等待其退出」的真实路径。
    auto warm = g_bbt_stackpoll->Acquire({kLarge, true});
    BOOST_REQUIRE(warm);
    warm.reset();

    detail::Stack::_TestFailNextAlloc();
    BOOST_CHECK_THROW(pool::CoPool::Create(Options({{{kLarge, true}, 2, 1}})), std::bad_alloc);
    BOOST_REQUIRE(Await([&] {
        const auto s = g_bbt_stackpoll->Observe();
        return s.borrowed == 0 && s.pending == 0;
    }));
    const auto after = g_bbt_stackpoll->Observe();
    BOOST_CHECK_EQUAL(after.live, 1u); // 只剩预热的那份缓存，回滚未泄漏
    BOOST_CHECK_EQUAL(after.committed_usable_bytes, kLarge);

    // 回滚后可重新创建同一规格的池并真实执行任务。
    auto pool = pool::CoPool::Create(Options({{{kLarge, true}, 2, 1}}));
    std::atomic_bool ran{false};
    BOOST_CHECK(pool->Submit({kLarge, true}, [&ran] { ran.store(true); }) ==
                pool::CoPoolSubmitStatus::Accepted);
    BOOST_REQUIRE(Await([&] { return ran.load(); }));
    pool->Release();
}

/** 单 Processer 推进：两类 worker 在同一执行单元上交错，不需要额外线程。 */
BOOST_AUTO_TEST_CASE(single_processer_progress) {
    StartRuntime();
    auto pool = pool::CoPool::Create(Options({
        {{kSmall, true}, 1, 1},
        {{kLarge, false}, 1, 1},
    }));

    std::atomic_bool small_ready{false}, large_done{false};
    std::atomic_size_t small_stack{0}, large_stack{0};
    BOOST_CHECK(pool->Submit({kSmall, true}, [&] {
        small_stack.store(GetLocalCoroutineStackSize());
        small_ready.store(true);
        while (!large_done.load())
            bbtco_sleep(1);
    }) == pool::CoPoolSubmitStatus::Accepted);
    BOOST_CHECK(pool->Submit({kLarge, false}, [&] {
        while (!small_ready.load())
            bbtco_sleep(1);
        large_stack.store(GetLocalCoroutineStackSize());
        large_done.store(true);
    }) == pool::CoPoolSubmitStatus::Accepted);

    BOOST_REQUIRE(Await([&] { return small_ready.load() && large_done.load(); }));
    BOOST_CHECK_EQUAL(small_stack.load(), kSmall);
    BOOST_CHECK_EQUAL(large_stack.load(), kLarge);
    pool->Release();
}

BOOST_AUTO_TEST_SUITE_END()
