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

        // Release 后 Submit 可能返回 0 但任务不会执行（池已停止）
        count = 0;
        pool->Submit([&](){ count++; });
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

BOOST_AUTO_TEST_CASE(t_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
