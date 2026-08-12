#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE()

BOOST_AUTO_TEST_CASE(t_begin)
{
    g_scheduler->Start();
}

BOOST_AUTO_TEST_CASE(t_test_copool)
{
    auto pool = bbtco_make_copool(100);
    const int max_co = 100000;
    bbt::core::thread::CountDownLatch l{max_co};
    std::atomic_int count = 0;

    for (int i = 0; i < max_co; ++i) {
        pool->Submit([&](){
            BOOST_ASSERT(GetLocalCoroutineId() > 0);
            count++;
            l.Down();
        });
    }

    l.Wait();

    BOOST_CHECK_EQUAL(count, max_co);
}

// =============== P1: Release 行为测试 ===============

// Release 后池停止接受新任务（协程内调用）
BOOST_AUTO_TEST_CASE(t_release_stops_pool)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco [&](){
        auto pool = bbtco_make_copool(10);

        // 提交一些任务并等待完成
        std::atomic_int count{0};
        bbt::core::thread::CountDownLatch done{10};
        for (int i = 0; i < 10; ++i) {
            pool->Submit([&](){ count++; done.Down(); });
        }
        done.Wait();
        BOOST_TEST(count == 10);

        // Release 停止池
        pool->Release();

        // Release 后 Submit 返回 -1（池已停止，拒绝入队）
        count = 0;
        BOOST_TEST(pool->Submit([&](){ count++; }) == -1);
        bbtco_sleep(50);
        BOOST_TEST(count == 0); // 池已停止，任务不会执行

        l.Down();
    };

    l.Wait();
}

// Release 后重复 Release 安全
BOOST_AUTO_TEST_CASE(t_double_release_safe)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco [&](){
        auto pool = bbtco_make_copool(10);
        pool->Release();
        // 第二次不应崩溃
        pool->Release();
        BOOST_TEST(true);
        l.Down();
    };

    l.Wait();
}

// 空池 Release 安全
BOOST_AUTO_TEST_CASE(t_release_empty_pool)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco [&](){
        auto pool = bbtco_make_copool(10);
        pool->Release();
        BOOST_TEST(true);
        l.Down();
    };

    l.Wait();
}

// 多个池独立 Release
BOOST_AUTO_TEST_CASE(t_multiple_pools_release)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco [&](){
        std::atomic_int count1{0};
        std::atomic_int count2{0};
        bbt::core::thread::CountDownLatch done1{100};
        bbt::core::thread::CountDownLatch done2{100};

        auto pool1 = bbtco_make_copool(20);
        auto pool2 = bbtco_make_copool(20);

        for (int i = 0; i < 100; ++i) {
            pool1->Submit([&](){ count1++; done1.Down(); });
            pool2->Submit([&](){ count2++; done2.Down(); });
        }

        done1.Wait();
        done2.Wait();

        // 释放 pool1
        pool1->Release();
        BOOST_TEST(count1 == 100);

        // pool2 仍可用
        std::atomic_int count2b{0};
        bbt::core::thread::CountDownLatch extra{50};
        for (int i = 0; i < 50; ++i)
            pool2->Submit([&](){ count2b++; extra.Down(); });
        extra.Wait();
        BOOST_TEST(count2b == 50);

        pool2->Release();
        l.Down();
    };

    l.Wait();
}

// Submit + Release 大量任务压力测试
BOOST_AUTO_TEST_CASE(t_stress_submit_release)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco [&](){
        for (int r = 0; r < 10; ++r) {
            auto pool = bbtco_make_copool(20);
            std::atomic_int count{0};
            bbt::core::thread::CountDownLatch done{500};

            for (int i = 0; i < 500; ++i)
                pool->Submit([&](){ count++; done.Down(); });

            done.Wait();
            BOOST_TEST(count == 500);
            pool->Release();
        }
        BOOST_TEST(true);
        l.Down();
    };

    l.Wait();
}

// =============== #189: 协程异常隔离测试 ===============

// 持续提交抛异常任务 + 正常任务交错，池持续工作、正常任务全部完成
BOOST_AUTO_TEST_CASE(t_throwing_tasks_pool_alive)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco [&](){
        auto pool = bbtco_make_copool(8);
        const int n_mix = 200;  // 抛异常与正常任务各 100，交错提交
        std::atomic_int ok_count{0};
        std::atomic_int throw_count{0};
        bbt::core::thread::CountDownLatch done{n_mix / 2};  // 仅正常任务 Down

        for (int i = 0; i < n_mix; ++i) {
            if (i % 2 == 0) {
                pool->Submit([&]() {
                    throw_count++;
                    throw std::runtime_error("boom");
                });
            } else {
                pool->Submit([&]() {
                    ok_count++;
                    done.Down();
                });
            }
        }

        done.Wait();
        BOOST_TEST(ok_count == n_mix / 2);
        BOOST_TEST(throw_count == n_mix / 2);

        // 异常被隔离后池仍可继续接收新任务
        std::atomic_int extra{0};
        bbt::core::thread::CountDownLatch extra_done{10};
        for (int i = 0; i < 10; ++i)
            pool->Submit([&](){ extra++; extra_done.Down(); });
        extra_done.Wait();
        BOOST_TEST(extra == 10);

        // 未设置回调：异常被计数
        BOOST_TEST(pool->GetUnhandledExceptionCount() == (uint64_t)(n_mix / 2));

        pool->Release();
        l.Down();
    };

    l.Wait();
}

// "忽略"策略默认计数观测
BOOST_AUTO_TEST_CASE(t_exception_ignore_count)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco [&](){
        auto pool = bbtco_make_copool(4);
        const int n = 25;
        bbt::core::thread::CountDownLatch done{n};

        for (int i = 0; i < n; ++i) {
            pool->Submit([&]() {
                done.Down();
                throw 42;  // 非 std 异常，catch(...) 需同样接住
            });
        }

        done.Wait();
        BOOST_TEST(pool->GetUnhandledExceptionCount() == (uint64_t)n);

        pool->Release();
        l.Down();
    };

    l.Wait();
}

// 回调策略：异常交付回调
BOOST_AUTO_TEST_CASE(t_exception_callback)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco [&](){
        auto pool = bbtco_make_copool(4);
        std::atomic_int cb_calls{0};
        std::atomic_bool got_runtime_error{false};
        bbt::core::thread::CountDownLatch done{1};

        pool->SetExceptionCallback([&](std::exception_ptr eptr) {
            cb_calls++;
            try {
                std::rethrow_exception(eptr);
            } catch (const std::runtime_error&) {
                got_runtime_error = true;
            } catch (...) {
            }
        });

        pool->Submit([&]() {
            done.Down();
            throw std::runtime_error("callback-me");
        });

        done.Wait();
        bbtco_sleep(50);
        BOOST_TEST(cb_calls.load() == 1);
        BOOST_TEST(got_runtime_error.load() == true);
        // 走回调路径的不计入 ignore 计数
        BOOST_TEST(pool->GetUnhandledExceptionCount() == 0);

        // 回调抛异常：隔离（仅计数），池不崩溃
        pool->SetExceptionCallback([&](std::exception_ptr) {
            throw std::runtime_error("callback itself throws");
        });
        bbt::core::thread::CountDownLatch done2{1};
        pool->Submit([&]() {
            done2.Down();
            throw std::runtime_error("x");
        });
        done2.Wait();
        bbtco_sleep(50);
        BOOST_TEST(pool->GetUnhandledExceptionCount() == 1);

        pool->Release();
        l.Down();
    };

    l.Wait();
}

// future 传递策略：SubmitWithFuture 交付异常与正常完成
BOOST_AUTO_TEST_CASE(t_submit_with_future)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco [&](){
        auto pool = bbtco_make_copool(4);

        // 正常任务：future.get() 正常返回
        std::atomic_int ok{0};
        auto f_ok = pool->SubmitWithFuture([&](){ ok++; });
        BOOST_TEST(f_ok.valid());
        f_ok.get();
        BOOST_TEST(ok == 1);

        // 异常任务：future.get() 抛出任务异常
        auto f_err = pool->SubmitWithFuture([]() {
            throw std::logic_error("future-err");
        });
        BOOST_TEST(f_err.valid());
        bool caught = false;
        try {
            f_err.get();
        } catch (const std::logic_error&) {
            caught = true;
        } catch (...) {
        }
        BOOST_TEST(caught == true);

        // future 路径不占用池级 ignore 计数
        BOOST_TEST(pool->GetUnhandledExceptionCount() == 0);

        pool->Release();
        l.Down();
    };

    l.Wait();
}

// Release 异常路径：抛异常任务后 Release 正常返回（不挂起）
BOOST_AUTO_TEST_CASE(t_release_after_throwing)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco [&](){
        for (int round = 0; round < 5; ++round) {
            auto pool = bbtco_make_copool(4);
            for (int i = 0; i < 20; ++i)
                pool->Submit([]() { throw std::runtime_error("x"); });

            bbtco_sleep(20);
            // 修复前：worker 协程异常逃逸终结 → m_latch 永不 Down → 挂起
            pool->Release();
            BOOST_TEST(true);
        }
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
