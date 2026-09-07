#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

// #280 Scheduler::Stop 停机契约：取消式停机
// ① Stop 等待 ≤ 有界时间（不阻塞等 parked 协程的业务完成）
// ② Stop 取消并唤醒全部等待事件（含 parked fd/timer/custom），随后全局队列排空销毁
// ③ 重复 Stop 安全且立即返回
// ④ 停机后 RegistCoroutineTask 明确失败（noexcept 版 succ=false，抛出版可捕获）

#include <atomic>
#include <chrono>
#include <cstdio>
#include <unistd.h>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(SchedulerStopContract)

namespace
{

struct CoFixture
{
    CoFixture()
    {
        auto* cfg = g_bbt_coroutine_config.get();
        m_threads = cfg->m_cfg_static_thread_num;
        m_stack = cfg->m_cfg_stack_size;
        m_protect = cfg->m_cfg_stack_protect;
        cfg->m_cfg_static_thread_num = 1;
        cfg->m_cfg_stack_size = 4096;
        cfg->m_cfg_stack_protect = false;
        // 用例自包含：先确保停机态再 Start，避免上个用例遗留的 running 态导致双 Start
        if (g_scheduler->IsRunning())
            g_scheduler->Stop();
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    }
    ~CoFixture()
    {
        if (g_scheduler->IsRunning())
            g_scheduler->Stop();
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = m_threads;
        cfg->m_cfg_stack_size = m_stack;
        cfg->m_cfg_stack_protect = m_protect;
    }
    int m_threads;
    size_t m_stack;
    bool m_protect;
};

long StopElapsedMs()
{
    const auto t0 = std::chrono::steady_clock::now();
    g_scheduler->Stop();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[stop-probe] elapsed=%ldms\n", ms);
    std::fflush(stdout);
    return ms;
}

} // namespace

// parked 协程（pipe 无数据的无限协程等待）不阻塞停机
BOOST_AUTO_TEST_CASE(t_stop_wakes_parked_and_is_bounded)
{
    CoFixture fx;
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);

    std::atomic_int parked{0};
    bbtco [&]() {
        parked.fetch_add(1);
        char buf[1];
        BOOST_CHECK_EQUAL(::read(fds[0], buf, 1), -1);   // Stop 后不得继续挂死
    };
    bbtco [&]() {
        parked.fetch_add(1);
        bbtco_sleep(60000);                              // 1 分钟定时器，停机必须取消
    };
    while (parked.load() < 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const long ms = StopElapsedMs();
    BOOST_CHECK_MESSAGE(ms >= 0 && ms < 2000,
                        "Stop with parked coroutines took " << ms << "ms (contract: bounded)");
    ::close(fds[0]);
    ::close(fds[1]);
}

// 重复 Stop：立即返回、安全
BOOST_AUTO_TEST_CASE(t_double_stop_is_safe)
{
    CoFixture fx;
    std::atomic_int parked{0};
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::pipe(fds), 0);
    bbtco [&]() {
        parked.fetch_add(1);
        char buf[1];
        ::read(fds[0], buf, 1);
    };
    while (parked.load() < 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    BOOST_CHECK(StopElapsedMs() < 2000);
    const long again = StopElapsedMs();
    BOOST_CHECK_MESSAGE(again < 100, "second Stop took " << again << "ms, must be immediate no-op");
    ::close(fds[0]);
    ::close(fds[1]);
}

// 停机后注册：noexcept 版 succ=false 不抛；抛出版异常可捕获（不得裸 abort）
BOOST_AUTO_TEST_CASE(t_stop_rejects_new_tasks)
{
    CoFixture fx;
    g_scheduler->Stop();

    bool succ = true;
    g_scheduler->RegistCoroutineTask([&]() { BOOST_FAIL("task must not run after Stop"); }, succ);
    BOOST_CHECK_EQUAL(succ, false);

    bool terminated = false;
    try {
        g_scheduler->RegistCoroutineTask([&]() { BOOST_FAIL("task must not run after Stop"); });
    } catch (...) {
        terminated = true;
    }
    BOOST_CHECK_MESSAGE(terminated, "throwing Regist after Stop must raise a catchable error, not abort");
}

BOOST_AUTO_TEST_SUITE_END()
