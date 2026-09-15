#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <thread>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

/**
 * @file Test_worker_tls_boundary.cc
 * @brief 跨 worker 恢复与用户 TLS 边界的正式回归（#339 验收切片）。
 *
 * 契约依据（agent-docs/2026-09-07-core-runtime-contract.md §2 执行模型）：
 *  - 支持多 worker 并行，单个协程同一时刻只能在一个 worker 上运行；
 *  - 协程挂起后允许恢复到其他 worker（"允许"，不是"必须"）。
 *
 * 因此本文件把断言分成两类，避免把契约没承诺的行为写成硬要求：
 *  1. 硬不变量（无论是否迁移都必须成立）：运行时 TLS 在每次 Resume 后指向协程自身、
 *     协程 ID 跨挂起稳定、同一协程不并发运行。
 *  2. 观测型存活断言（当前实现的行为，注释里标明上限）：事件完成后恢复确实会发生
 *     跨 worker 迁移，且用户 thread_local 是 worker 作用域而非协程作用域。这两条用于
 *     保证"迁移路径"和"用户 TLS 边界"被真正执行到；若将来实现改为固定 worker 亲和，
 *     需要按契约重新评估这两条，而不是直接改契约。
 *
 * 用户 TLS 边界（本文件要固定下来的结论）：
 *  - 运行时 TLS（g_bbt_tls_processer / g_bbt_tls_coroutine_co）由运行时在每次 Resume
 *    前更新，协程迁移后仍指向当前协程。
 *  - 用户 thread_local 属于 **worker 线程**：同一 worker 上的不同协程共享同一份实例。
 *    因此"用户 thread_local 的值在挂起期间保持不变"不成立——同 worker 上被其他协程
 *    改写后会变化，迁移到其他 worker 后看到的也是那份线程实例的值。跨挂起依赖
 *    用户 thread_local 的代码需要在三平台分别复核。
 */

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

namespace
{

constexpr int kWorkerCount = 2;
/* 4 个协程在两个 worker 上轮转注册，保证同一 worker 上必然先后运行多个协程，
 * 用户 TLS 共享因此可被稳定观察到。 */
constexpr int kYieldCoroutines = 4;
constexpr int kYieldIterations = 32;
constexpr int kYieldTimeoutMs = 10000;
/* 40 轮：迁移是竞争结果（探针阶段 24 轮观测 14~20 次，本文件修复后 12 次复跑观测
 * 25~35 次），把"整段一次迁移都没有"的概率压到可忽略。 */
constexpr int kEventRounds = 40;
constexpr int kEventWaitTimeoutMs = 2000;
/* park 后先让该 worker 有机会空闲：经验上这样唤醒后的协程更常被另一个 worker 取走
 * （本地 40 轮观测 25~35 次跨 worker 恢复）。该参数只影响迁移出现频率，
 * 不影响任何硬不变量断言。 */
constexpr int kParkIdleMs = 5;
/* Notify 在 COND_WAIT 未发布时返回 -1，被抢占的 worker 可能超过 kParkIdleMs；
 * 有界重试，避免把调度延迟误报成运行时等待缺陷。 */
constexpr int kNotifyRetryLimit = 500;

struct ConfigSnapshot
{
    size_t threads{0};
    size_t stack_size{0};
    bool stack_protect{false};
    bool saved{false};
};

ConfigSnapshot g_cfg;
std::atomic_bool g_started{false};

/* 用户 thread_local：故意只按线程存"最后写入者"，用来观察作用域。 */
thread_local uint64_t tl_last_writer_co = 0;
thread_local uint64_t tl_last_writer_worker = 0;

struct YieldStats
{
    std::atomic<uint64_t> resumes{0};
    std::atomic<uint64_t> tls_bad{0};
    std::atomic<uint64_t> migrations{0};
    std::atomic<uint64_t> user_tls_cross_coroutine_share{0};
    std::atomic<uint64_t> self_concurrent{0};
    std::mutex            workers_mu;
    std::set<ProcesserId> workers;
};

ProcesserId LocalWorkerId()
{
    auto proc = Processer::GetLocalProcesser();
    return proc ? proc->GetId() : 0;
}

struct EventStats
{
    std::atomic<uint64_t> rounds{0};
    std::atomic<uint64_t> migrated{0};
    std::atomic<uint64_t> tls_bad{0};
    std::atomic<uint64_t> wait_failed{0};
    std::atomic<uint64_t> resume_timeout{0};
    std::atomic<uint64_t> notify_failed{0};
    std::atomic<uint64_t> coroutine_not_started{0};
};

/* 每轮状态用堆对象持有：协程按值捕获 shared_ptr，等待超时后恢复也不会访问悬垂对象。 */
struct EventRoundState
{
    bbt::core::thread::CountDownLatch parked{1};
    bbt::core::thread::CountDownLatch resumed{1};
    std::atomic<ProcesserId> parked_worker{0};
    std::atomic<ProcesserId> resume_worker{0};
    std::atomic<int>         wait_ret{99};
    std::atomic<uint64_t>    tls_bad{0};
};

/**
 * @brief 运行 kYieldCoroutines 个协程各自让出 kYieldIterations 次。
 *
 * 时序约束：统计用的互斥锁必须在 bbtco_yield 之前释放——协程持
 * std::mutex 挂起会让下一个申请同一把锁的协程把 worker 线程永久阻塞，
 * 运行时不会替用户解开（这是用户侧用法约束，不是运行时缺陷）。
 */
bool RunYieldPhase(YieldStats& stats)
{
    bbt::core::thread::CountDownLatch done{kYieldCoroutines};

    for (int i = 0; i < kYieldCoroutines; ++i) {
        bbtco [&, i]() {
            const CoroutineId self = GetLocalCoroutineId();
            if (self == 0) {
                stats.tls_bad.fetch_add(1, std::memory_order_relaxed);
                done.Down();
                return;
            }

            ProcesserId last_worker = 0;
            std::atomic<int> inside{0};
            std::atomic<int> inside_max{0};

            for (int k = 0; k < kYieldIterations; ++k) {
                const int now = inside.fetch_add(1, std::memory_order_acq_rel) + 1;
                int seen = inside_max.load(std::memory_order_relaxed);
                while (now > seen && !inside_max.compare_exchange_weak(seen, now)) {}

                const ProcesserId worker = LocalWorkerId();
                auto* cur = g_bbt_tls_coroutine_co;
                /* cur->GetId() 与 GetLocalCoroutineId() 读的是同一份 worker TLS
                 * （Define.hpp 的 g_bbt_tls_coroutine_co），不是两条独立判据；
                 * 同时保留是为了覆盖"直接读 TLS"与"公共 API"两个入口都不残留旧值。 */
                if (cur == nullptr || cur->GetId() != self ||
                    GetLocalCoroutineId() != self ||
                    cur->GetStatus() != CoroutineStatus::CO_RUNNING ||
                    worker == 0) {
                    stats.tls_bad.fetch_add(1, std::memory_order_relaxed);
                }

                /* 用户 TLS 判据：thread_local 属于线程，所以本处读到的
                 * tl_last_writer_worker 必然等于当前 worker（prev_worker == worker 恒真，
                 * 保留该条件只是显式表达"共享的是同一线程实例"）。真正的判据是
                 * 上一次写入者是同一 worker 上的另一个协程。 */
                const uint64_t prev_co = tl_last_writer_co;
                const uint64_t prev_worker = tl_last_writer_worker;
                tl_last_writer_co = self;
                tl_last_writer_worker = worker;
                if (prev_co != 0 && prev_co != self && prev_worker == worker) {
                    stats.user_tls_cross_coroutine_share.fetch_add(1, std::memory_order_relaxed);
                }

                stats.resumes.fetch_add(1, std::memory_order_relaxed);
                if (last_worker != 0 && worker != last_worker)
                    stats.migrations.fetch_add(1, std::memory_order_relaxed);
                last_worker = worker;
                {
                    std::lock_guard<std::mutex> lk(stats.workers_mu);
                    stats.workers.insert(worker);
                }

                /* 同一协程并发运行的探测点：上一次 Resume 未退出就再次进入。
                 * 这是采样型探测——只能发现"被观察到的并发窗口"，不能证明不存在并发；
                 * 断言措辞据此写为"未观测到同协程并发"。 */
                if (inside_max.load(std::memory_order_relaxed) > 1)
                    stats.self_concurrent.fetch_add(1, std::memory_order_relaxed);

                inside.fetch_sub(1, std::memory_order_acq_rel);
                bbtco_yield;
            }

            if (inside_max.load(std::memory_order_relaxed) > 1)
                stats.self_concurrent.fetch_add(1, std::memory_order_relaxed);
            done.Down();
        };
    }

    return done.WaitTimeout(kYieldTimeoutMs) == 0;
}

} // namespace

BOOST_AUTO_TEST_SUITE(WorkerTlsBoundaryTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    auto* cfg = g_bbt_coroutine_config.get();
    if (!g_cfg.saved) {
        g_cfg.threads = cfg->m_cfg_static_thread_num;
        g_cfg.stack_size = cfg->m_cfg_stack_size;
        g_cfg.stack_protect = cfg->m_cfg_stack_protect;
        g_cfg.saved = true;
    }

    cfg->m_cfg_static_thread_num = kWorkerCount;
    cfg->m_cfg_stack_protect = false;
    /* 64 KiB 保护栈：本文件验证跨 worker/TLS 语义，不叠加 #337 的小栈（默认 12 KiB）配置问题。 */
    cfg->m_cfg_stack_size = 64 * 1024;

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    g_started.store(true);
    BOOST_REQUIRE(g_scheduler->IsRunning());
}

/**
 * 让出路径：每次 Resume 上运行时 TLS 都必须指向协程自身，且用户 thread_local
 * 表现为 worker 作用域。
 */
BOOST_AUTO_TEST_CASE(t_runtime_tls_stable_across_yields)
{
    BOOST_REQUIRE(g_started.load());

    YieldStats stats;
    BOOST_REQUIRE_MESSAGE(RunYieldPhase(stats), "yield 阶段未在有界时间内完成");

    const uint64_t resumes = stats.resumes.load();
    BOOST_CHECK_EQUAL(resumes, static_cast<uint64_t>(kYieldCoroutines * kYieldIterations));
    /* 硬不变量：运行时 TLS 与状态在每次 Resume 上都必须正确。 */
    BOOST_CHECK_EQUAL(stats.tls_bad.load(), 0u);
    /* 硬不变量：未观测到同一协程并发运行（采样型探测，见 RunYieldPhase 注释）。 */
    BOOST_CHECK_EQUAL(stats.self_concurrent.load(), 0u);
    /* 参与调度的 worker 数：这里只作健壮性下界（>=1）。
     * 真正的跨 worker 覆盖由事件阶段的 migrated > 0 提供——yield 阶段是否发生迁移
     * 由调度竞争决定（修复后 12 次复跑观测 0~8/128），不适合写成硬断言。 */
    size_t participating_workers = 0;
    {
        std::lock_guard<std::mutex> lk(stats.workers_mu);
        participating_workers = stats.workers.size();
    }
    BOOST_CHECK_GE(participating_workers, 1u);
    /* 用户 TLS 边界：同 worker 上的其他协程能看到本协程写入的值，
     * 即 thread_local 不按协程隔离。 */
    BOOST_CHECK_MESSAGE(stats.user_tls_cross_coroutine_share.load() > 0,
        "未观察到用户 thread_local 在同一 worker 的不同协程之间共享；"
        "若 thread_local 变成按协程隔离，用户 TLS 边界结论需要重新评估");

    BOOST_TEST_MESSAGE("yield 阶段：resumes=" << resumes
        << " workers=" << participating_workers
        << " migrations=" << stats.migrations.load()
        << " user_tls_shared=" << stats.user_tls_cross_coroutine_share.load());
}

/**
 * 事件完成路径：协程在 worker 上 park，被外部线程唤醒后可能在其他 worker 上恢复。
 * 每次恢复仍然必须满足运行时 TLS 不变量，并且至少观察到一次跨 worker 恢复。
 *
 * 时序约束（两条都来自实现事实，不能省）：
 *  1. CoWaiter::Notify() 在 m_run_status 尚未进入 COND_WAIT 时返回 -1 并丢弃唤醒
 *     （CoWaiter.cc Notify 的 m_co_event/m_run_status 判据），而 COND_WAIT 是在协程
 *     进入 WaitWithTimeout 后才发布。因此固定 sleep 不足以覆盖被抢占的 worker，
 *     这里用有界重试 + Notify 返回值断言，避免把调度延迟误报成运行时等待缺陷。
 *  2. 先让 park 所在 worker 有机会进入空闲；经验上这提高"唤醒后被另一个 worker
 *     从全局队列取走"的比例（本地观测 25~35/40），本文件的跨 worker 覆盖依赖它。
 *
 * 轮次状态用 shared_ptr 持有并被协程按值捕获：即使某一轮在等待超时后才恢复，
 * 协程访问的仍是有效对象，不会出现悬垂引用。
 */
BOOST_AUTO_TEST_CASE(t_runtime_tls_stable_after_event_wake)
{
    BOOST_REQUIRE(g_started.load());

    EventStats stats;

    for (int round = 0; round < kEventRounds; ++round) {
        auto state = std::make_shared<EventRoundState>();
        auto waiter = sync::CoWaiter::Create();

        bbtco [state, waiter]() {
            const CoroutineId self = GetLocalCoroutineId();
            state->parked_worker.store(LocalWorkerId());
            state->parked.Down();
            state->wait_ret.store(waiter->WaitWithTimeout(kEventWaitTimeoutMs));

            auto* cur = g_bbt_tls_coroutine_co;
            const ProcesserId worker = LocalWorkerId();
            /* 同 yield 阶段：两个 id 判据同源（worker TLS），保留用于覆盖公共 API 入口。 */
            if (cur == nullptr || cur->GetId() != self ||
                GetLocalCoroutineId() != self ||
                cur->GetStatus() != CoroutineStatus::CO_RUNNING ||
                worker == 0) {
                state->tls_bad.fetch_add(1);
            }
            state->resume_worker.store(worker);
            state->resumed.Down();
        };

        if (state->parked.WaitTimeout(5000) != 0) {
            stats.coroutine_not_started.fetch_add(1);
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kParkIdleMs));

        int notify_ret = -1;
        for (int i = 0; i < kNotifyRetryLimit && (notify_ret = waiter->Notify()) != 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (notify_ret != 0) {
            /* Notify 全程返回 -1：意味协程在被重试窗口内没有进入等待（被饿死），
             * 或等待已进入终态。两种情况都不应发生，失败信息按读数区分：
             * coroutine_not_started == 0 说明协程已开始运行，即落在这里。 */
            stats.notify_failed.fetch_add(1);
            break;
        }

        if (state->resumed.WaitTimeout(5000) != 0) {
            stats.resume_timeout.fetch_add(1);
            break;
        }

        if (state->wait_ret.load() != 0)
            stats.wait_failed.fetch_add(1);
        if (state->tls_bad.load() != 0)
            stats.tls_bad.fetch_add(1);
        if (state->parked_worker.load() != 0 && state->resume_worker.load() != 0 &&
            state->parked_worker.load() != state->resume_worker.load()) {
            stats.migrated.fetch_add(1);
        }
        stats.rounds.fetch_add(1);
    }

    BOOST_CHECK_EQUAL(stats.coroutine_not_started.load(), 0u);
    BOOST_CHECK_EQUAL(stats.notify_failed.load(), 0u);
    BOOST_CHECK_EQUAL(stats.resume_timeout.load(), 0u);
    BOOST_CHECK_EQUAL(stats.wait_failed.load(), 0u);
    BOOST_CHECK_EQUAL(stats.tls_bad.load(), 0u);
    BOOST_CHECK_EQUAL(stats.rounds.load(), static_cast<uint64_t>(kEventRounds));
    /* 观测型断言（上限已注明）：当前实现把唤醒后的协程放回全局队列，由任意 worker
     * 取走，因此事件完成后跨 worker 恢复是常态行为。本条要求"至少观测到一次"，
     * 用于保证迁移路径被真正执行；契约只要求"允许恢复其他 worker"，
     * 若将来改为固定 worker 亲和，应调整本条而不是改契约。 */
    BOOST_CHECK_MESSAGE(stats.migrated.load() > 0,
        "未观测到事件完成后跨 worker 恢复；跨 worker 迁移路径未被覆盖");

    BOOST_TEST_MESSAGE("事件唤醒阶段：rounds=" << stats.rounds.load()
        << " migrated=" << stats.migrated.load()
        << " (跨 worker 恢复占比 " << stats.migrated.load() << "/" << kEventRounds << ")");
}

BOOST_AUTO_TEST_CASE(t_end)
{
    if (g_started.exchange(false))
        g_scheduler->Stop();

    if (g_cfg.saved) {
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = g_cfg.threads;
        cfg->m_cfg_stack_size = g_cfg.stack_size;
        cfg->m_cfg_stack_protect = g_cfg.stack_protect;
        g_cfg.saved = false;
    }
}

BOOST_AUTO_TEST_SUITE_END()
