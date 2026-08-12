#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/coroutine.hpp>

namespace
{

// 带 deadline 的 latch 轮询等待。
// 不依赖 CountDownLatch::WaitTimeout：bbtools-core 旧版本该接口存在缺陷
// （end_tm 未加 timeout，恒立即超时），而测试进程链接的 core 版本不可控
// （CI runner 系统库）。用 WaitTimeout(1) 作非阻塞探测 + 外层 deadline 轮询。
void WaitLatchWithDeadline(bbt::core::thread::CountDownLatch& latch, int timeout_ms)
{
    auto deadline = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(timeout_ms));
    while (!bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(deadline)) {
        if (latch.WaitTimeout(1) == 0)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    BOOST_ERROR("latch wait timed out");
}

} // namespace

BOOST_AUTO_TEST_SUITE(TestDefer)

BOOST_AUTO_TEST_CASE(t_begin)
{
    g_scheduler->Start();
}

BOOST_AUTO_TEST_CASE(t_defer)
{
    int i = 0;

    bbtco_defer {
        BOOST_CHECK(i == 0);
    };

    bbtco_defer {
        BOOST_CHECK(i == 2);
        i = 0;
    };
    
    bbtco_defer {
        BOOST_CHECK(i == 1);
        i = 2;
    };

    bbtco_defer {
        BOOST_CHECK(i == 0);
        i = 1;
    };

}

// #193: 异常抛出路径 defer 仍执行（栈展开触发析构）
BOOST_AUTO_TEST_CASE(t_defer_on_exception)
{
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int defer_executed{0};

    bbtco [&]() {
        bbtco_defer {
            defer_executed++;
            done.Down();
        };
        throw std::runtime_error("defer-unwind");
    };

    // defer 在异常栈展开时执行，done.Down 后返回
    WaitLatchWithDeadline(done, 5000);
    BOOST_TEST(defer_executed.load() == 1);
}

// #193: 多个 defer 在异常路径上的 LIFO 顺序
BOOST_AUTO_TEST_CASE(t_defer_lifo_on_exception)
{
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int seq{0};
    std::atomic_int order[3] = {0, 0, 0};

    bbtco [&]() {
        bbtco_defer {
            order[seq++] = 1;  // 第三个注册，第一个执行
            done.Down();
        };
        bbtco_defer {
            order[seq++] = 2;  // 第二个注册，第二个执行
        };
        bbtco_defer {
            order[seq++] = 3;  // 第一个注册，最后执行
        };
        throw std::runtime_error("lifo-unwind");
    };

    WaitLatchWithDeadline(done, 5000);
    // LIFO：执行顺序 3 → 2 → 1
    BOOST_TEST(order[0] == 3);
    BOOST_TEST(order[1] == 2);
    BOOST_TEST(order[2] == 1);
}

// #193: 嵌套协程 + defer + 异常传播——内外层 defer 均执行
BOOST_AUTO_TEST_CASE(t_defer_nested_coroutine_exception)
{
    bbt::core::thread::CountDownLatch done{2};
    std::atomic_int defer_executed{0};

    bbtco [&]() {
        bbtco_defer {
            defer_executed++;
            done.Down();
        };

        bbtco [&]() {
            bbtco_defer {
                defer_executed++;
                done.Down();
            };
            throw std::runtime_error("inner-unwind");
        };
    };

    WaitLatchWithDeadline(done, 5000);
    BOOST_TEST(defer_executed.load() == 2);
}

// #193: CoPool 任务异常路径的 defer（配合 #189 异常隔离）
BOOST_AUTO_TEST_CASE(t_defer_in_copool_exception)
{
    bbt::core::thread::CountDownLatch done{1};
    std::atomic_int defer_executed{0};

    auto pool = bbtco_make_copool(4);
    pool->Submit([&]() {
        bbtco_defer {
            defer_executed++;
            done.Down();
        };
        throw std::runtime_error("copool-unwind");
    });

    WaitLatchWithDeadline(done, 5000);
    BOOST_TEST(defer_executed.load() == 1);

    // 池仍正常工作
    std::atomic_int ok{0};
    bbt::core::thread::CountDownLatch ok_done{1};
    pool->Submit([&]() { ok++; ok_done.Down(); });
    WaitLatchWithDeadline(ok_done, 5000);
    BOOST_TEST(ok.load() == 1);

    pool->Release();
}

BOOST_AUTO_TEST_CASE(t_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
