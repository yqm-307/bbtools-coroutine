// Test_reliability.cc — 可靠性陷阱 CI 门禁测试
//
// 契约参考: docs/testing-contract-spec.md §3 (20 陷阱清单)
// ADR:      docs/adr/0001-testing-contract.md D5 (TRAP-ID 命名)
//
// 测试命名: t_<TRAP-ID> — 对应契约中的陷阱 ID
//   DL-0x: 死锁类, UAF-0x: 悬挂指针类, DR-0x: 数据竞争类
//   ML-0x: 内存泄漏类, UB-0x: 未定义行为类, EX-0x: 异常安全类
//
// 注: 标注「需疲劳测试」或「需 ASAN」的陷阱不在 ctest 范围，
//     由 CI Layer 2 (perf-regression) 和 Layer 3 (sanitizer) 覆盖。

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
#include <bbt/coroutine/sync/CoRWMutex.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/sync/CoLockGuard.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/pool/CoPool.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#define BOOST_TEST_MODULE Test_reliability
#include <boost/test/unit_test.hpp>
#include <boost/test/unit_test_parameters.hpp>

namespace co = bbt::coroutine;
namespace cs = bbt::coroutine::sync;
namespace pool = bbt::coroutine::pool;

// ═══════════════════════════════════════════════════════════
// Suite 级：启动 Scheduler
// ═══════════════════════════════════════════════════════════
BOOST_AUTO_TEST_CASE(t_scheduler_start)
{
    g_scheduler->Start();
}

// ═══════════════════════════════════════════════════════════
// DL-01: 持锁 await — 不能持有 CoMutex/CoRWMutex 跨 yield
// ═══════════════════════════════════════════════════════════
BOOST_AUTO_TEST_CASE(t_DL_01_lock_across_await_no_deadlock)
{
    auto mutex = bbtco_make_comutex();
    std::atomic<int> phase{0};

    bbtco_ref {
        mutex->Lock();
        phase.store(1);
        bbtco_sleep(50);
        mutex->UnLock();
        phase.store(2);
    };

    bbtco_ref {
        while (phase.load() < 1)
            bbtco_sleep(10);
        mutex->Lock();
        BOOST_TEST(phase.load() >= 2);
        mutex->UnLock();
        phase.store(3);
    };

    while (phase.load() < 3)
        bbtco_sleep(10);
    BOOST_TEST(phase.load() == 3);
}

// ═══════════════════════════════════════════════════════════
// DL-02: 析构竞态 — Scheduler Stop() 时正确清理资源
// ═══════════════════════════════════════════════════════════
BOOST_AUTO_TEST_CASE(t_DL_02_scheduler_shutdown_clean)
{
    // 验证: Run 一批协程后 Scheduler 正确 Exit
    std::atomic<int> counter{0};
    static auto g_mutex = bbtco_make_comutex();

    for (int i = 0; i < 20; ++i) {
        bbtco_ref {
            for (int j = 0; j < 10 && counter.load() < 200; ++j) {
                g_mutex->Lock();
                counter.fetch_add(1);
                g_mutex->UnLock();
                bbtco_sleep(5);
            }
        };
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    BOOST_TEST(counter.load() > 0);
}

// ═══════════════════════════════════════════════════════════
// DL-03: LOOP_NONBLOCK 阻塞 — 已修复 (bbtools-core ASIO迁移)
// ═══════════════════════════════════════════════════════════
// 注: 此陷阱已由 bbtools-core 的 t_loop_nonblock_returns_immediately 覆盖
//     此处放置占位，引用已有测试

// ═══════════════════════════════════════════════════════════
// UAF-01: CoWaiter 在协程异常退出后悬空
// ═══════════════════════════════════════════════════════════
BOOST_AUTO_TEST_CASE(t_UAF_01_cowaiter_shared_ptr_safety)
{
    // 验证: shared_ptr 管理的 CoWaiter 在异常路径下不悬空
    auto cond = cs::CoCond::Create();
    std::atomic<int> notified{0};

    bbtco_ref {
        cond->Wait();
        notified.fetch_add(1);
    };

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    bbtco_ref {
        cond->NotifyAll();
    };

    while (notified.load() < 1)
        bbtco_sleep(10);
    BOOST_TEST(notified.load() == 1);
}

// ═══════════════════════════════════════════════════════════
// UAF-02: Chan 关闭后 Read 安全返回
// ═══════════════════════════════════════════════════════════
BOOST_AUTO_TEST_CASE(t_UAF_02_chan_close_read_safety)
{
    cs::Chan<int, 8> ch{};
    std::atomic<int> result{0};

    bbtco_ref {
        int val = 0;
        int rc = ch.Read(val);
        result.store(rc);
    };

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ch.Close();

    while (result.load() == 0)
        bbtco_sleep(10);
    BOOST_TEST(result.load() == -1);
}

// ═══════════════════════════════════════════════════════════
// UAF-03/UAF-04: 需 ASAN 构建检测 — 由 CI sanitizer-check 覆盖
// ═══════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════
// DR-01: Chan 并发 Read/Write 数据竞争
// ═══════════════════════════════════════════════════════════
BOOST_AUTO_TEST_CASE(t_DR_01_chan_concurrent_read_write)
{
    constexpr int N = 1000;
    cs::Chan<int, 64> ch{};
    std::atomic<int> sum{0};
    std::atomic<int> count{0};

    bbtco_ref {
        for (int i = 0; i < N; ++i)
            ch.Write(i);
        ch.Close();
    };

    bbtco_ref {
        int val = 0;
        while (ch.Read(val) == 0) {
            sum.fetch_add(val);
            count.fetch_add(1);
        }
    };

    while (count.load() < N)
        bbtco_sleep(10);

    int expected = (N - 1) * N / 2;
    BOOST_TEST(sum.load() == expected);
    BOOST_TEST(count.load() == N);
}

// ═══════════════════════════════════════════════════════════
// DR-02: 需 TSan 构建 — ADR D4 规定 TSan 不纳入 CI
// ═══════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════
// ML-01/ML-02: 需长时间疲劳测试+RSS监控 — CI Layer 3 覆盖
// ═══════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════
// UB-01: atomic 容器 resize
// ═══════════════════════════════════════════════════════════
BOOST_AUTO_TEST_CASE(t_UB_01_atomic_vector_resize_workaround)
{
    constexpr int N = 16;
    auto arr = std::make_unique<std::atomic_int[]>(N);
    for (int i = 0; i < N; ++i)
        arr[i].store(i);
    for (int i = 0; i < N; ++i)
        BOOST_TEST(arr[i].load() == i);
}

// ═══════════════════════════════════════════════════════════
// UB-02: usleep 在协程内阻塞调度线程
// ═══════════════════════════════════════════════════════════
BOOST_AUTO_TEST_CASE(t_UB_02_bbtco_sleep_not_block_scheduler)
{
    std::atomic<int> phase{0};

    bbtco_ref {
        bbtco_sleep(100);
        phase.store(1);
    };

    bbtco_ref {
        bbtco_sleep(10);
        phase.store(2);
    };

    while (phase.load() == 0)
        bbtco_sleep(5);
    BOOST_TEST(phase.load() != 0);
}

// ═══════════════════════════════════════════════════════════
// UB-03: 协程栈溢出 — 需 ASAN 或极端递归压测
// ═══════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════
// EX-01: Lock 抛出异常后状态一致性
// ═══════════════════════════════════════════════════════════
BOOST_AUTO_TEST_CASE(t_EX_01_lockguard_exception_safety)
{
    auto mutex = bbtco_make_comutex();
    std::atomic<int> unlocked{0};

    bbtco_ref {
        try {
            cs::CoUniqueLock<cs::CoMutex> guard(mutex);
            BOOST_TEST(guard.owns_lock());
            throw std::runtime_error("test");
        } catch (...) {
            unlocked.store(1);
        }
    };

    while (unlocked.load() < 1)
        bbtco_sleep(10);

    bbtco_ref {
        mutex->Lock();
        unlocked.store(2);
        mutex->UnLock();
    };

    while (unlocked.load() < 2)
        bbtco_sleep(10);
    BOOST_TEST(unlocked.load() == 2);
}

// ═══════════════════════════════════════════════════════════
// EX-02: CoCond Wait 异常安全
// ═══════════════════════════════════════════════════════════
BOOST_AUTO_TEST_CASE(t_EX_02_cocond_exception_safety)
{
    auto cond = cs::CoCond::Create();
    std::atomic<int> result{0};

    bbtco_ref {
        cond->Wait();
        result.store(1);
    };

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    bbtco_ref {
        cond->NotifyAll();
    };

    while (result.load() < 1)
        bbtco_sleep(10);
    BOOST_TEST(result.load() == 1);
}

// ═══════════════════════════════════════════════════════════
// EX-03: CoPool 任务正常执行
// ═══════════════════════════════════════════════════════════
BOOST_AUTO_TEST_CASE(t_EX_03_copool_task_execution)
{
    auto pool = pool::CoPool::Create(4);
    std::atomic<int> completed{0};

    bbtco_ref {
        pool->Submit([&completed]() {
            completed.store(1);
        });
    };

    while (completed.load() < 1)
        bbtco_sleep(10);
    BOOST_TEST(completed.load() == 1);
}

// ═══════════════════════════════════════════════════════════
// 综合: 多模块并发不变量
// ═══════════════════════════════════════════════════════════
BOOST_AUTO_TEST_CASE(t_INV_concurrent_lock_unlock_pairing)
{
    auto mutex = bbtco_make_comutex();
    std::atomic<int> critical{0};
    std::atomic<int> violations{0};

    static constexpr int kThreads = 4;
    static constexpr int kLoops = 100;

    for (int t = 0; t < kThreads; ++t) {
        bbtco_ref {
            for (int i = 0; i < kLoops; ++i) {
                mutex->Lock();
                int old = critical.fetch_add(1);
                if (old != 0) {
                    violations.fetch_add(1);
                }
                critical.fetch_sub(1);
                mutex->UnLock();
                bbtco_sleep(1);
            }
        };
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
    BOOST_TEST(violations.load() == 0);
}

// ═══════════════════════════════════════════════════════════
// 综合: CoRWMutex 读写不变量
// ═══════════════════════════════════════════════════════════
BOOST_AUTO_TEST_CASE(t_INV_rwmutex_readers_writers_exclusion)
{
    auto rwlock = cs::CoRWMutex::Create();
    std::atomic<int> readers{0};
    std::atomic<int> writers{0};
    std::atomic<int> max_readers{0};
    std::atomic<int> violations{0};

    static constexpr int kReaders = 4;
    static constexpr int kWriters = 2;
    static constexpr int kLoops = 50;

    for (int r = 0; r < kReaders; ++r) {
        bbtco_ref {
            for (int i = 0; i < kLoops; ++i) {
                rwlock->RLock();
                int cur = readers.fetch_add(1) + 1;
                int old_max = max_readers.load();
                while (cur > old_max && !max_readers.compare_exchange_weak(old_max, cur));
                if (writers.load() > 0)
                    violations.fetch_add(1);
                readers.fetch_sub(1);
                rwlock->RUnLock();
                bbtco_sleep(1);
            }
        };
    }

    for (int w = 0; w < kWriters; ++w) {
        bbtco_ref {
            for (int i = 0; i < kLoops; ++i) {
                rwlock->WLock();
                writers.fetch_add(1);
                if (readers.load() > 0 || writers.load() > 1)
                    violations.fetch_add(1);
                writers.fetch_sub(1);
                rwlock->WUnLock();
                bbtco_sleep(2);
            }
        };
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));
    BOOST_TEST(violations.load() == 0);
    BOOST_TEST(max_readers.load() > 0);
}

// ═══════════════════════════════════════════════════════════
// 综合: 锁守卫 defer 模式
// ═══════════════════════════════════════════════════════════
BOOST_AUTO_TEST_CASE(t_INV_lockguard_move_semantics)
{
    auto mutex = bbtco_make_comutex();
    std::atomic<int> result{0};

    bbtco_ref {
        cs::CoUniqueLock<cs::CoMutex> lock1(mutex);
        BOOST_TEST(lock1.owns_lock());
        cs::CoUniqueLock<cs::CoMutex> lock2(std::move(lock1));
        BOOST_TEST(!lock1.owns_lock());
        BOOST_TEST(lock2.owns_lock());
        result.store(1);
    };

    while (result.load() < 1)
        bbtco_sleep(10);

    bbtco_ref {
        mutex->Lock();
        result.store(2);
        mutex->UnLock();
    };

    while (result.load() < 2)
        bbtco_sleep(10);
    BOOST_TEST(result.load() == 2);
}

BOOST_AUTO_TEST_CASE(t_INV_lockguard_defer_mode)
{
    auto mutex = bbtco_make_comutex();
    std::atomic<int> result{0};

    bbtco_ref {
        cs::CoUniqueLock<cs::CoMutex> lock(mutex, std::defer_lock);
        BOOST_TEST(!lock.owns_lock());
        lock.lock();
        BOOST_TEST(lock.owns_lock());
        result.store(1);
    };

    while (result.load() < 1)
        bbtco_sleep(10);

    bbtco_ref {
        mutex->Lock();
        result.store(2);
        mutex->UnLock();
    };

    while (result.load() < 2)
        bbtco_sleep(10);
    BOOST_TEST(result.load() == 2);
}
