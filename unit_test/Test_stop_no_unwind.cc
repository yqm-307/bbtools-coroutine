/**
 * @file Test_stop_no_unwind.cc
 * @brief #338 语义回归测试：Stop 对挂起协程直接销毁、不做栈展开
 *
 * 契约：agent-docs/2026-09-07-core-runtime-contract.md §6
 * 挂起协程在 Stop 时被直接销毁，栈上对象不执行析构与 RAII。
 */

#include <atomic>
#include <chrono>
#include <thread>

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE(StopNoUnwind)

/* 栈上 RAII 哨兵：析构时置标志 */
struct StackGuard {
    std::atomic<int>* destroyed;
    explicit StackGuard(std::atomic<int>* d) : destroyed(d) {}
    ~StackGuard() { destroyed->fetch_add(1); }
};

BOOST_AUTO_TEST_CASE(t_stop_parked_coroutine_no_stack_unwind)
{
    std::atomic<int> guard_destroyed{0};
    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};

    g_bbt_coroutine_config->m_cfg_stack_size = 16384;
    g_bbt_coroutine_config->m_cfg_stack_protect = true;

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    BOOST_REQUIRE(g_scheduler->IsRunning());

    bbtco [&]() {
        started.store(true);
        StackGuard guard(&guard_destroyed);
        /* 挂起在超时事件上，永远不会自然超时 */
        g_bbt_tls_coroutine_co->YieldUntilTimeout(60000);
        /* 如果 Stop 后协程被恢复执行到这里，说明做了栈展开——不应该发生 */
        completed.store(true);
    };

    /* 等协程真正启动并挂起 */
    for (int i = 0; i < 200 && !started.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_REQUIRE(started.load());
    /* 再等一拍确保进入 PARKED */
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    g_scheduler->Stop();

    /* Stop 后挂起协程被直接销毁：协程未恢复执行，StackGuard 析构不应执行 */
    BOOST_CHECK(!completed.load());
    BOOST_CHECK_EQUAL(guard_destroyed.load(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
