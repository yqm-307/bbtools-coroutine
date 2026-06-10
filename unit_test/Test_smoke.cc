/**
 * @file Test_smoke.cc
 * @brief 冒烟测试 — 编译后第一道快速检查，验证核心模块无崩溃
 *
 * 冒烟测试设计原则：
 *  - 每个模块只测最基本 happy-path（创建、使用、销毁）
 *  - 全量运行 < 5秒
 *  - 不测边界、并发、压力（留给单元测试/疲劳测试）
 *  - 任一用例失败 = 库有根本性问题，不应继续跑其他测试
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

/* =========================================================================
   Scheduler — 协程调度器启停 & 注册协程任务
   ========================================================================= */
BOOST_AUTO_TEST_SUITE(Smoke_Scheduler)

BOOST_AUTO_TEST_CASE(smoke_scheduler_start_stop)
{
    auto& sche = detail::Scheduler::GetInstance();
    BOOST_REQUIRE(sche != nullptr);

    // Stop first to ensure clean state (may have been started by earlier tests)
    sche->Stop();
    sche->Start();
    BOOST_CHECK(sche->IsRunning());
    sche->Stop();
    BOOST_CHECK(!sche->IsRunning());
}

BOOST_AUTO_TEST_CASE(smoke_scheduler_regist_run_coroutine)
{
    auto& sche = detail::Scheduler::GetInstance();
    sche->Stop();
    sche->Start();

    std::atomic_int counter{0};

    sche->RegistCoroutineTask([&counter]() {
        counter++;
    });

    // Give scheduler time to execute the task
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sche->Stop();

    BOOST_CHECK_GE(counter, 1);
}

BOOST_AUTO_TEST_SUITE_END()

/* =========================================================================
   Chan — 有缓冲 / 无缓冲通道基本读写
   ========================================================================= */
BOOST_AUTO_TEST_SUITE(Smoke_Chan)

BOOST_AUTO_TEST_CASE(smoke_chan_buffered_write_read)
{
    auto& sche = detail::Scheduler::GetInstance();
    sche->Stop();
    sche->Start();

    auto ch = Chan<int, 8>();

    std::atomic_bool done{false};

    sche->RegistCoroutineTask([&]() {
        int val = 42;
        int ret = ch->Write(val);
        BOOST_CHECK_EQUAL(ret, 0);
        done = true;
    });

    sche->RegistCoroutineTask([&]() {
        int val = 0;
        int ret = ch->Read(val);
        BOOST_CHECK_EQUAL(ret, 0);
        BOOST_CHECK_EQUAL(val, 42);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sche->Stop();

    BOOST_CHECK(done);
}

BOOST_AUTO_TEST_CASE(smoke_chan_unbuffered_write_read)
{
    auto& sche = detail::Scheduler::GetInstance();
    sche->Stop();
    sche->Start();

    auto ch = Chan<int, 0>();

    std::atomic_int result{0};

    sche->RegistCoroutineTask([&]() {
        int val = 0;
        int ret = ch->Read(val);
        if (ret == 0)
            result = val;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    sche->RegistCoroutineTask([&]() {
        int val = 99;
        ch->Write(val);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sche->Stop();

    BOOST_CHECK_EQUAL(result, 99);
}

BOOST_AUTO_TEST_CASE(smoke_chan_close_empty)
{
    auto ch = Chan<int, 4>();
    BOOST_CHECK(!ch->IsClosed());
    ch->Close();
    BOOST_CHECK(ch->IsClosed());
}

BOOST_AUTO_TEST_SUITE_END()

/* =========================================================================
   CoMutex — 协程互斥锁
   ========================================================================= */
BOOST_AUTO_TEST_SUITE(Smoke_CoMutex)

BOOST_AUTO_TEST_CASE(smoke_comutex_lock_unlock)
{
    auto& sche = detail::Scheduler::GetInstance();
    sche->Stop();
    sche->Start();

    auto mtx = sync::CoMutex::Create();

    std::atomic_int counter{0};

    sche->RegistCoroutineTask([&]() {
        mtx->Lock();
        counter++;
        mtx->UnLock();
    });

    sche->RegistCoroutineTask([&]() {
        mtx->Lock();
        counter++;
        mtx->UnLock();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sche->Stop();

    BOOST_CHECK_EQUAL(counter, 2);
}

BOOST_AUTO_TEST_CASE(smoke_comutex_trylock_immediate)
{
    auto& sche = detail::Scheduler::GetInstance();
    sche->Stop();
    sche->Start();

    auto mtx = sync::CoMutex::Create();
    std::atomic_bool trylock_ok{false};

    // TryLock must be called from within a coroutine (has deadlock assertion)
    sche->RegistCoroutineTask([&]() {
        int ret = mtx->TryLock();
        if (ret == 0) {
            trylock_ok = true;
            mtx->UnLock();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sche->Stop();

    BOOST_CHECK(trylock_ok);
}

BOOST_AUTO_TEST_SUITE_END()

/* =========================================================================
   CoCond — 协程条件变量
   ========================================================================= */
BOOST_AUTO_TEST_SUITE(Smoke_CoCond)

BOOST_AUTO_TEST_CASE(smoke_cocond_wait_notify_one)
{
    auto& sche = detail::Scheduler::GetInstance();
    sche->Stop();
    sche->Start();

    auto cond = sync::CoCond::Create();
    std::atomic_bool woken{false};

    sche->RegistCoroutineTask([&]() {
        int ret = cond->Wait();
        if (ret == 0)
            woken = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    cond->NotifyOne();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sche->Stop();

    BOOST_CHECK(woken);
}

BOOST_AUTO_TEST_SUITE_END()

/* =========================================================================
   CoRWMutex — 读写锁
   ========================================================================= */
BOOST_AUTO_TEST_SUITE(Smoke_CoRWMutex)

BOOST_AUTO_TEST_CASE(smoke_corwmutex_read_lock_unlock)
{
    auto& sche = detail::Scheduler::GetInstance();
    sche->Stop();
    sche->Start();

    auto rwmtx = sync::CoRWMutex::Create();

    std::atomic_int read_count{0};

    sche->RegistCoroutineTask([&]() {
        rwmtx->RLock();
        read_count++;
        rwmtx->RUnLock();
    });

    sche->RegistCoroutineTask([&]() {
        rwmtx->RLock();
        read_count++;
        rwmtx->RUnLock();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sche->Stop();

    BOOST_CHECK_EQUAL(read_count, 2);
}

BOOST_AUTO_TEST_CASE(smoke_corwmutex_write_lock_unlock)
{
    auto& sche = detail::Scheduler::GetInstance();
    sche->Stop();
    sche->Start();

    auto rwmtx = sync::CoRWMutex::Create();
    std::atomic_int shared{0};

    sche->RegistCoroutineTask([&]() {
        rwmtx->WLock();
        shared = 42;
        rwmtx->WUnLock();
    });

    sche->RegistCoroutineTask([&]() {
        rwmtx->WLock();
        shared = shared + 1;
        rwmtx->WUnLock();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sche->Stop();

    BOOST_CHECK_EQUAL(shared, 43);
}

BOOST_AUTO_TEST_SUITE_END()

/* =========================================================================
   CoPool — 协程池
   ========================================================================= */
BOOST_AUTO_TEST_SUITE(Smoke_CoPool)

BOOST_AUTO_TEST_CASE(smoke_copool_submit_and_release)
{
    auto& sche = detail::Scheduler::GetInstance();
    sche->Stop();
    sche->Start();

    auto pool = pool::CoPool::Create(2);
    std::atomic_int counter{0};

    pool->Submit([&counter]() {
        counter++;
    });

    pool->Submit([&counter]() {
        counter++;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    pool->Release();

    sche->Stop();

    BOOST_CHECK_EQUAL(counter, 2);
}

BOOST_AUTO_TEST_SUITE_END()

/* =========================================================================
   Hook — sleep 钩子
   ========================================================================= */
BOOST_AUTO_TEST_SUITE(Smoke_Hook)

BOOST_AUTO_TEST_CASE(smoke_hook_sleep)
{
    auto& sche = detail::Scheduler::GetInstance();
    sche->Stop();
    sche->Start();

    std::atomic_bool slept{false};

    sche->RegistCoroutineTask([&]() {
        // bbtco_sleep uses the hook mechanism to yield instead of blocking the thread
        bbtco_sleep(10);
        slept = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sche->Stop();

    BOOST_CHECK(slept);
}

BOOST_AUTO_TEST_SUITE_END()

/* =========================================================================
   Defer — 延迟执行
   ========================================================================= */
BOOST_AUTO_TEST_SUITE(Smoke_Defer)

BOOST_AUTO_TEST_CASE(smoke_defer_execute)
{
    auto& sche = detail::Scheduler::GetInstance();
    sche->Stop();
    sche->Start();

    std::atomic_bool defer_executed{false};

    sche->RegistCoroutineTask([&]() {
        bbtco_defer {
            defer_executed = true;
        };
        // defer fires when this coroutine exits
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sche->Stop();

    BOOST_CHECK(defer_executed);
}

BOOST_AUTO_TEST_SUITE_END()
