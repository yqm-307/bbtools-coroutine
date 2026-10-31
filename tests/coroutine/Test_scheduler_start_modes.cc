/**
 * @file Test_scheduler_start_modes.cc
 * @brief 三种 Start 驱动模式各在独立进程内验证可观察时序；真实并发 Start
 *        一次成功其余 std::logic_error；正常进程 exit 与静态不析构链。
 *
 * 为什么 fork 独立进程：Start 一次性、驱动模式不能同进程切换（重复 Start
 * 抛 std::logic_error），worker 数也只在首次 Start 之前可配置。fork 让每个
 * 模式在干净子进程里成为「首次 Start」，父进程只收退出码，互不污染。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <thread>

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(SchedulerStartModesTest)

namespace
{

int RunInChild(const std::function<int()>& body)
{
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        /* 子进程：只做被测动作，_exit 跳过 boost/静态析构，退出码即结论 */
        const int rc = body();
        ::_exit(rc);
    }
    int status = 0;
    BOOST_REQUIRE_EQUAL(::waitpid(pid, &status, 0), pid);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;   // 被信号终止（崩溃/超时）时返回 -1
}

bool WaitFlag(const std::atomic_bool& flag, int ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/* 退出标记：atexit 需要普通函数指针，故用文件级 fd + 无捕获函数。 */
namespace {
int g_exit_marker_fd = -1;
void WriteExitMarker()
{
    const char flag = 'E';
    const ssize_t n = ::write(g_exit_marker_fd, &flag, 1);
    (void)n;
}
} // namespace

/* 子进程走**真实进程退出路径**：body() 返回后调用 std::exit（与从 main
 * 返回同一套 __run_exit_handlers：atexit 处理器 + 静态对象析构），而不是
 * _exit。子进程在退出处理阶段往管道写一个标记字节，父进程读到才认为
 * 「析构链确实执行过」——否则用例只是又验证了一次退出码 0（r1 已证 _exit
 * 让静态析构从未执行，是假验证）。
 * 返回：>=0 为子进程退出码；-1 异常终止（信号）；-2 退出处理器未执行。 */
int RunInChildRealExit(const std::function<int()>& body)
{
    int marker[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::pipe(marker), 0);
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(marker[0]);
        g_exit_marker_fd = marker[1];
        std::atexit(WriteExitMarker);
        const int rc = body();
        std::exit(rc);          // 非 _exit：执行真实退出/静态析构链
    }
    ::close(marker[1]);
    char got = 0;
    const ssize_t nread = ::read(marker[0], &got, 1);
    ::close(marker[0]);
    int status = 0;
    BOOST_REQUIRE_EQUAL(::waitpid(pid, &status, 0), pid);
    if (nread != 1 || got != 'E')
        return -2;               // 退出处理器未执行：验收不成立
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;                   // 被信号终止（崩溃/超时）
}

/* 主线程侧有界等待一个协程结果（协程不在本线程，可以阻塞） */
template<class T>
bool WaitValue(const std::atomic<T>& value, const T& expected, int ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (value.load(std::memory_order_acquire) != expected) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

/* ① THREAD：Start 立即返回且已发布初始化；注册的协程真实执行 */
BOOST_AUTO_TEST_CASE(t_thread_mode_independent_process)
{
    const int rc = RunInChild([]() -> int {
        g_bbt_coroutine_config->m_cfg_static_thread_num = 1;
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
        if (!g_scheduler->IsInitialized())
            return 10;
        std::atomic_bool ran{false};
        bbtco [&]() { ran.store(true, std::memory_order_release); };
        return WaitFlag(ran, 3000) ? 0 : 11;
    });
    BOOST_CHECK_EQUAL(rc, 0);
}

/* ② NO_LOOP：Start 返回并发布初始化；纯 CPU 协程不需要 LoopOnce 也能跑
 *     （LoopOnce 只驱动事件循环）。反向判别：**到期定时器只有在 LoopOnce
 *     推进时才收敛**——deadline 早已过期但未驱动时等待者仍挂起，显式
 *     LoopOnce 后才得到 TimedOut，证明 NO_LOOP 下没有隐藏的驱动线程。 */
BOOST_AUTO_TEST_CASE(t_noloop_mode_independent_process)
{
    const int rc = RunInChild([]() -> int {
        g_bbt_coroutine_config->m_cfg_static_thread_num = 1;
        g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
        if (!g_scheduler->IsInitialized())
            return 20;
        std::atomic_bool ran{false};
        bbtco [&]() { ran.store(true, std::memory_order_release); };
        if (!WaitFlag(ran, 3000))
            return 21;

        auto waiter = sync::CoWaiter::Create();
        std::atomic_int resolved{-1};
        bbtco [&]() {
            WaitOptions options;
            options.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{60};
            const auto st = waiter->Wait(options);
            resolved.store(st == WaitStatus::TimedOut ? 1 : 2);
        };
        /* 远远超过 deadline 也不得收敛：没有 LoopOnce 就没有事件循环推进 */
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (resolved.load() != -1)
            return 22;
        for (int i = 0; i < 200 && resolved.load() == -1; ++i) {
            g_scheduler->LoopOnce();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (resolved.load() != 1)
            return 23;
        return 0;
    });
    BOOST_CHECK_EQUAL(rc, 0);
}

/* ③ LOOP：Start 阻塞在当前线程驱动调度；观察线程确认初始化已发布、协程被
 *     执行，并确认**到期定时器由 LOOP 自己的事件循环推进**（本线程无法调用
 *     LoopOnce，Start 正阻塞在它的 _Run 里），随后 _exit 收尾——本用例只
 *     验证驱动，退出路径由 ⑤ 用真实 std::exit 覆盖。 */
BOOST_AUTO_TEST_CASE(t_loop_mode_independent_process)
{
    const int rc = RunInChild([]() -> int {
        g_bbt_coroutine_config->m_cfg_static_thread_num = 1;
        std::atomic_bool ran{false};
        std::atomic_int timer_state{-1};
        std::thread observer([&]() {
            /* Start 阻塞在 LOOP 模式，注册只能在初始化发布之后由另一线程做 */
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!g_scheduler->IsInitialized()) {
                if (std::chrono::steady_clock::now() > deadline)
                    ::_exit(30);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            bbtco [&]() { ran.store(true, std::memory_order_release); };
            if (!WaitFlag(ran, 3000))
                ::_exit(31);   // 初始化已发布但调度未推进：LOOP 驱动失败

            auto waiter = sync::CoWaiter::Create();
            bbtco [&]() {
                WaitOptions options;
                options.deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds{80};
                const auto st = waiter->Wait(options);
                timer_state.store(st == WaitStatus::TimedOut ? 1 : 2);
            };
            if (!WaitValue(timer_state, 1, 3000))
                ::_exit(33);   // LOOP 未驱动到期定时器
            ::_exit(0);
        });
        g_scheduler->Start(SCHE_START_OPT_SCHE_LOOP);   // 阻塞：由 observer 结束进程
        observer.detach();
        return 32;                                       // 理论上不可达
    });
    BOOST_CHECK_EQUAL(rc, 0);
}

/* ④ 真实并发 Start：所有线程在屏障后同时起跑，恰好一次成功，其余
 *    std::logic_error；不重置配置/队列。无屏障时首个线程可能已完成 Start，
 *    用例退化为「各自调用结果」，证据强度不足（r1 已指出）。 */
BOOST_AUTO_TEST_CASE(t_concurrent_start_one_wins)
{
    const int rc = RunInChild([]() -> int {
        g_bbt_coroutine_config->m_cfg_static_thread_num = 2;
        constexpr int kThreads = 4;
        std::atomic_int ok{0}, rejected{0}, unexpected{0};
        std::atomic_int ready{0};
        std::atomic_bool go{false};
        {
            std::thread ts[kThreads];
            for (int i = 0; i < kThreads; ++i) {
                ts[i] = std::thread([&]() {
                    ready.fetch_add(1, std::memory_order_release);
                    while (!go.load(std::memory_order_acquire))
                        std::this_thread::yield();       // 屏障：同时起跑
                    try {
                        g_scheduler->Start();
                        ok.fetch_add(1);
                    } catch (const std::logic_error&) {
                        rejected.fetch_add(1);
                    } catch (...) {
                        unexpected.fetch_add(1);
                    }
                });
            }
            while (ready.load(std::memory_order_acquire) != kThreads)
                std::this_thread::yield();
            go.store(true, std::memory_order_release);
            for (auto& t : ts)
                t.join();
        }
        if (ok.load() != 1 || rejected.load() != kThreads - 1 || unexpected.load() != 0)
            return 40;
        return g_scheduler->IsInitialized() ? 0 : 41;
    });
    BOOST_CHECK_EQUAL(rc, 0);
}

/* ⑤ 正常进程退出：挂起中的协程 + 已启动运行时 + 已登记的 fd 等待项，
 *    走**真实进程退出路径**（子进程 std::exit：atexit 处理器 + 静态对象
 *    析构，不是 _exit）。父进程必须读到退出处理阶段的标记字节，证明析构链
 *    确实执行过（r1 已证 _exit 让该用例只验证了退出码）。
 *    worker 可达静态（含 CoPollEvent 的 fd→epoch/waiter 注册表）按进程寿命
 *    持有、不在静态退出期析构，故退出期不得崩溃/被信号终止。 */
BOOST_AUTO_TEST_CASE(t_process_exit_with_parked_coroutine)
{
    const int rc = RunInChildRealExit([]() -> int {
        g_bbt_coroutine_config->m_cfg_static_thread_num = 1;
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
        std::atomic_bool parked{false};
        std::atomic_bool fd_parked{false};
        auto waiter = sync::CoWaiter::Create();
        bbtco [&]() {
            parked.store(true, std::memory_order_release);
            waiter->Wait();   // 无 deadline：进程寿命内一直挂起
        };
        if (!WaitFlag(parked, 3000))
            return 50;

        /* 再挂一个 fd 等待项：让 CoPollEvent 的静态 fd 注册表在退出时非空，
         * 覆盖 worker 可达静态的寿命链。 */
        int sp[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0)
            return 51;
        bbtco [&]() {
            char c = 0;
            fd_parked.store(true, std::memory_order_release);
            ::read(sp[0], &c, 1);   // 无数据：park 在 fd 事件上
        };
        if (!WaitFlag(fd_parked, 3000))
            return 52;
        for (int i = 0; i < 3000 && CoPollEvent::FdWaiterCount(sp[0]) == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (CoPollEvent::FdWaiterCount(sp[0]) == 0)
            return 53;          // fd 等待项未登记，寿命链覆盖不成立

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return 0;   // → 子进程 std::exit(0)：真实退出/静态析构链
    });
    BOOST_CHECK_EQUAL(rc, 0);
}

BOOST_AUTO_TEST_SUITE_END()
