#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <bbt/coroutine/coroutine.hpp>
#include <memory>
#include <thread>

namespace bbt::coroutine::sync
{

class CoRWMutexTestAccess
{
public:
    static void SetWaitCallbacks(CoRWMutex& rwlock,
                                 const std::function<void()>& writer_queued,
                                 const std::function<void()>& reader_blocked)
    {
        rwlock.m_writer_queued_callback = writer_queued;
        rwlock.m_reader_blocked_callback = reader_blocked;
    }
};

}

BOOST_AUTO_TEST_SUITE(CoRWMutexTest)

BOOST_AUTO_TEST_CASE(t_start_scheduler)
{
    // 使用安全的协程栈大小，避免 ASAN 检测到的 stack-buffer-overflow
    // （默认 12KB 在某些协程操作中不足，导致间歇性 crash）
    g_bbt_coroutine_config->m_cfg_stack_size = 64 * 1024;  // 64KB
    g_scheduler->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

/* 测试读锁非阻塞 */
BOOST_AUTO_TEST_CASE(t_rlock_block)
{
    bbt::core::thread::CountDownLatch l{1};
    bool succ = false;

    bbtco [&](){
        auto cocond = bbt::coroutine::sync::CoCond::Create();
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();
        bbtco [=, &succ](){
            rwlock->RLock();
            succ = true;
            cocond->Wait();
            rwlock->RUnLock();
        };

        bbtco_sleep(50);

        bbtco [=, &succ](){

            bbtco_sleep(50);
            rwlock->RLock();
            BOOST_ASSERT(succ == true);
            cocond->NotifyAll();
            rwlock->RUnLock();
        };

        cocond->Wait();
        l.Down();
    };
    
    sleep(1);
    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_rlock_wlock_block)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main co") [&](){
        int n = 0, m = 0;
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();
        bbtco [&](){
            rwlock->RLock();
            n++;
        };

        bbtco [&](){
            m++;
            rwlock->WLock();
            n++;
        };

        bbtco_sleep(100);
        BOOST_ASSERT(n == m && n == 1);
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_rwlock_multi_co)
{
    std::atomic_bool running = true;
    bbt::core::thread::CountDownLatch l{1};
    int nwrite = 0;
    int nread = 0;

    bbtco_desc("main") [&](){
        std::atomic_bool in_writing{false};
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();
        bbtco_desc("reader1") [&]() {
            while (running) {
                rwlock->RLock();
                BOOST_ASSERT(in_writing == false);
                nread++;
                rwlock->RUnLock();
                bbtco_yield;
            }
        };

        bbtco_desc("reader2") [&]() {
            while (running) {
                rwlock->RLock();
                BOOST_ASSERT(in_writing == false);
                nread++;
                rwlock->RUnLock();
            }
            bbtco_yield;
        };

        bbtco_desc("writer") [&]() {
            while (running) {
                rwlock->WLock();
                in_writing = true;
                in_writing = false;
                nwrite++;
                rwlock->WUnLock();
            }
        };

        bbtco_sleep(1000);
        running = false;
        bbtco_sleep(100);
        l.Down();
    };

    l.Wait();
}

/* 测试写优先：有 writer 排队时，新 reader 不得获取锁 */
BOOST_AUTO_TEST_CASE(t_writer_priority)
{
    struct CaseState
    {
        bbt::coroutine::sync::CoRWMutex::SPtr rwlock{
            bbt::coroutine::sync::CoRWMutex::Create()};
        bbt::core::thread::CountDownLatch completed{1};
        std::atomic_bool reader_holding{false};
        std::atomic_bool writer_queued{false};
        std::atomic_bool reader_blocked{false};
        std::atomic_bool release_reader{false};
        std::atomic_bool writer_finished{false};
        std::atomic_bool reader_acquired_after_writer{false};
        std::atomic_int child_completed{0};
    };
    auto state = std::make_shared<CaseState>();
    std::weak_ptr<CaseState> weak_state{state};
    bbt::coroutine::sync::CoRWMutexTestAccess::SetWaitCallbacks(*state->rwlock,
        [weak_state] {
            if (auto shared_state = weak_state.lock())
                shared_state->writer_queued.store(true, std::memory_order_release);
        },
        [weak_state] {
            if (auto shared_state = weak_state.lock())
                shared_state->reader_blocked.store(true, std::memory_order_release);
        });

    bbtco [state] {
        state->rwlock->RLock();
        state->reader_holding.store(true, std::memory_order_release);
        while (!state->release_reader.load(std::memory_order_acquire))
            bbtco_yield;
        state->rwlock->RUnLock();
        state->child_completed.fetch_add(1, std::memory_order_release);
    };

    bbtco [state] {
        while (!state->reader_holding.load(std::memory_order_acquire))
            bbtco_yield;
        state->rwlock->WLock();
        state->writer_finished.store(true, std::memory_order_release);
        state->rwlock->WUnLock();
        state->child_completed.fetch_add(1, std::memory_order_release);
    };

    bbtco [state] {
        while (!state->writer_queued.load(std::memory_order_acquire))
            bbtco_yield;
        state->rwlock->RLock();
        state->reader_acquired_after_writer.store(
            state->writer_finished.load(std::memory_order_acquire),
            std::memory_order_release);
        state->rwlock->RUnLock();
        state->child_completed.fetch_add(1, std::memory_order_release);
    };

    bbtco [state] {
        while (!state->reader_blocked.load(std::memory_order_acquire))
            bbtco_yield;
        state->release_reader.store(true, std::memory_order_release);
        while (state->child_completed.load(std::memory_order_acquire) != 3)
            bbtco_yield;
        state->completed.Down();
    };

    state->completed.Wait();
    BOOST_CHECK(state->writer_finished.load(std::memory_order_acquire));
    BOOST_CHECK(state->reader_acquired_after_writer.load(std::memory_order_acquire));
}

/* 测试 TryRLock 非阻塞获取 */
BOOST_AUTO_TEST_CASE(t_try_rlock)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&](){
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        BOOST_ASSERT(rwlock->TryRLock() == 0);
        rwlock->RUnLock();

        rwlock->WLock();
        BOOST_ASSERT(rwlock->TryRLock() == -1);
        rwlock->WUnLock();

        l.Down();
    };

    l.Wait();
}

/* 测试 TryWLock 非阻塞获取 */
BOOST_AUTO_TEST_CASE(t_try_wlock)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&](){
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        BOOST_ASSERT(rwlock->TryWLock() == 0);
        rwlock->WUnLock();

        rwlock->RLock();
        BOOST_ASSERT(rwlock->TryWLock() == -1);
        rwlock->RUnLock();

        l.Down();
    };

    l.Wait();
}

/* 测试 TryRLock(ms) 在 writer 持有时超时 */
BOOST_AUTO_TEST_CASE(t_try_rlock_timeout)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&](){
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        rwlock->WLock();
        int ret = rwlock->TryRLock(50);
        BOOST_ASSERT(ret == 1);
        rwlock->WUnLock();

        BOOST_ASSERT(rwlock->TryRLock(100) == 0);
        rwlock->RUnLock();

        l.Down();
    };

    l.Wait();
}

/* 测试 TryWLock(ms) 在 reader 持有时超时 */
BOOST_AUTO_TEST_CASE(t_try_wlock_timeout)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&](){
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        rwlock->RLock();
        int ret = rwlock->TryWLock(50);
        BOOST_ASSERT(ret == 1);
        rwlock->RUnLock();

        BOOST_ASSERT(rwlock->TryWLock(100) == 0);
        rwlock->WUnLock();

        l.Down();
    };

    l.Wait();
}

/* 测试 timeout 后继续正常使用锁 */
BOOST_AUTO_TEST_CASE(t_timeout_recovery)
{
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_int n{0};

    bbtco_desc("main") [&](){
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        rwlock->WLock();

        bbtco_desc("timeout_reader") [rwlock]() {
            int ret = rwlock->TryRLock(50);
            BOOST_ASSERT(ret == 1);
        };

        bbtco_sleep(100);
        rwlock->WUnLock();

        bbtco_sleep(50);

        bbtco_desc("later_reader") [rwlock, &n]() {
            rwlock->RLock();
            n++;
            rwlock->RUnLock();
        };

        bbtco_sleep(100);
        BOOST_ASSERT(n == 1);
        l.Down();
    };

    l.Wait();
}

// =================== P0: TryWLock(ms) 超时后脏状态验证 ===================

// TryWLock(ms) 超时后 WLock 仍可正常获取（m_has_wait_wlock 标志正确清除）
BOOST_AUTO_TEST_CASE(t_try_wlock_timeout_then_wlock)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&](){
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        // 持有读锁
        rwlock->RLock();

        // TryWLock 超时
        int ret = rwlock->TryWLock(50);
        BOOST_ASSERT(ret == 1);

        // 释放读锁后，WLock 应该可以正常获取（m_has_wait_wlock 已清除）
        rwlock->RUnLock();

        bbtco_sleep(20);
        rwlock->WLock();
        rwlock->WUnLock();

        BOOST_TEST(true);
        l.Down();
    };

    l.Wait();
}

// TryWLock(ms) 超时后 reader 不受影响（m_has_wait_wlock 标志正确清除）
BOOST_AUTO_TEST_CASE(t_try_wlock_timeout_then_rlock)
{
    bbt::core::thread::CountDownLatch l{1};
    int nread = 0;

    bbtco_desc("main") [&](){
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        // 持有读锁
        rwlock->RLock();

        // TryWLock 超时
        int ret = rwlock->TryWLock(50);
        BOOST_ASSERT(ret == 1);

        rwlock->RUnLock();

        // 其他 reader 应该可以正常获取读锁（不被假 writer 阻塞）
        bbtco [rwlock, &nread]() {
            rwlock->RLock();
            nread++;
            rwlock->RUnLock();
        };

        bbtco_sleep(50);
        BOOST_TEST(nread == 1);
        l.Down();
    };

    l.Wait();
}

// 多次 TryWLock(ms) 超时后锁仍正常工作
BOOST_AUTO_TEST_CASE(t_try_wlock_multiple_timeouts)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&](){
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        // 持有读锁，连续多次 TryWLock 超时
        rwlock->RLock();

        for (int i = 0; i < 5; ++i) {
            int ret = rwlock->TryWLock(20);
            BOOST_ASSERT(ret == 1);
        }

        rwlock->RUnLock();

        // 释放后所有操作正常
        BOOST_ASSERT(rwlock->TryWLock() == 0);
        rwlock->WUnLock();

        BOOST_ASSERT(rwlock->TryRLock() == 0);
        rwlock->RUnLock();

        BOOST_TEST(true);
        l.Down();
    };

    l.Wait();
}

/* 增强多 reader / 多 writer 压力测试 */
BOOST_AUTO_TEST_CASE(t_rwlock_multi_co_stress)
{
    std::atomic_bool running{true};
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_int nread{0};
    std::atomic_int nwrite{0};

    bbtco_desc("main") [&](){
        std::atomic_int in_writing{0};
        std::atomic_int active_readers{0};
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        for (int i = 0; i < 4; ++i) {
            bbtco [&]() {
                while (running) {
                    rwlock->RLock();
                    active_readers.fetch_add(1);
                    BOOST_ASSERT(in_writing == 0);
                    nread.fetch_add(1);
                    active_readers.fetch_sub(1);
                    rwlock->RUnLock();
                    bbtco_yield;
                }
            };
        }

        for (int i = 0; i < 3; ++i) {
            bbtco [&]() {
                while (running) {
                    rwlock->WLock();
                    int old = in_writing.fetch_add(1);
                    BOOST_ASSERT(old == 0);
                    BOOST_ASSERT(active_readers == 0);
                    nwrite.fetch_add(1);
                    in_writing.fetch_sub(1);
                    rwlock->WUnLock();
                    bbtco_yield;
                }
            };
        }

        bbtco_sleep(500);
        running = false;
        bbtco_sleep(100);
        BOOST_ASSERT(nread > 0);
        BOOST_ASSERT(nwrite > 0);
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_stop_scheduler)
{
    g_scheduler->Stop();
    // 等待所有 processer 线程完全退出，避免 Boost.Test 全局析构竞态
    // （未加等待时 Test_co_rwmutex 间歇性 segfault @0x2b8, ~20% 失败率）
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

BOOST_AUTO_TEST_SUITE_END()
