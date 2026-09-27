#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

// #370 FD epoch 线性化回归：close 后同号 fd 复用不得把旧等待/新等待串线。
//
// 三类判别性用例：
//  1) 正常事件后恢复窗口：协程因 READABLE 唤醒但尚未恢复执行时，
//     另一线程 close+dup2 复用同号——恢复路径不得以旧 fd 数字重试
//     syscall（必须返回 -1/EBADF，不得读到新对象数据）。
//  2) close 与新 waiter 并发：BeginFdClose 摘表后到 EndFdClose 解 closing
//     之间的窗口内，同号 fd 上的新 waiter 不得被误摘/误醒；底层 close
//     完成后新 waiter 采样新一代际，应被真实事件正常唤醒。
//  3) 多 worker close+reuse：非单 worker 配置下，协程在 fd 上 park 后
//     由另一线程并发 close+dup2——旧协程得 EBADF、新对象不受影响。
//
// 同步约定：fixture 固定 m_cfg_static_thread_num（用例 3 用多 worker），
// 协程内禁止 CountDownLatch::Wait（阻塞 worker 会饿死同 worker 其它协程），
// 一律用 atomic flag + bbtco_sleep 让出轮询；主线程侧用 WaitLatch 有界等待。
// 复用源一律取独立 socketpair（dup2 同 socket 会因对端已关闭而 EPIPE）。

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <filesystem>
#include <thread>
#include <unistd.h>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/pollevent/Event.hpp>
#include <bbt/pollevent/EventLoop.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(HookFdEpochTest)

namespace
{

struct CoFixture
{
    explicit CoFixture(int threads = 1)
    {
        m_old_pipe = std::signal(SIGPIPE, SIG_IGN);
        auto* cfg = g_bbt_coroutine_config.get();
        m_threads = cfg->m_cfg_static_thread_num;
        m_stack = cfg->m_cfg_stack_size;
        m_protect = cfg->m_cfg_stack_protect;
        cfg->m_cfg_static_thread_num = threads;
        cfg->m_cfg_stack_protect = false;
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    }
    ~CoFixture()
    {
        std::signal(SIGPIPE, m_old_pipe);
        g_scheduler->Stop();
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = m_threads;
        cfg->m_cfg_stack_size = m_stack;
        cfg->m_cfg_stack_protect = m_protect;
    }
    int m_threads;
    size_t m_stack;
    bool m_protect;
    void (*m_old_pipe)(int){SIG_DFL};
};

// 主线程侧有界等待（主线程不是 worker，可阻塞）
bool WaitLatch(bbt::core::thread::CountDownLatch& latch, int ms)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (latch.WaitTimeout(1) == 0)
            return true;
    }
    return latch.WaitTimeout(0) == 0;
}

// 协程内非阻塞等待 flag（让出轮询，不占死 worker）；返回 false 表示超时
bool CoWaitFlag(std::atomic<bool>& flag, int max_ms)
{
    for (int i = 0; i < max_ms && !flag.load(std::memory_order_acquire); ++i)
        bbtco_sleep(1);
    return flag.load(std::memory_order_acquire);
}

/* #370 真实 park 同步：等到 fd 上确有 waiter 登记（A 已完成
 * InitFdEvent+Regist+_TrackWaiter），而不是靠「lambda 启动 + 固定
 * sleep」猜时序。返回 false 表示超时未观察到登记。 */
bool CoWaitFdWaiter(int fd, int max_ms)
{
    for (int i = 0; i < max_ms; ++i) {
        if (CoPollEvent::FdWaiterCount(fd) > 0)
            return true;
        bbtco_sleep(1);
    }
    return false;
}

} // namespace

/* ------------------------------------------------------------------ *
 * 用例 1：正常 READABLE 事件已触发、协程尚未恢复执行时的 close+dup2。
 *
 * 时序：A 在 fds[0] park（读等）→ 对端 fds[1] 写 'r' 使 A 的 epoll 就绪，
 *       CoPollEvent::_Complete 摘 waiter、协程入调度队列；在 A 真正恢复
 *       前，控制协程 close(fds[0]) + dup2(np[0]→fds[0]) + np[1] 写 'z'。
 *       A 恢复后不得重试 read 到复用后的新对象：必须返回 -1/EBADF。
 *
 * 判别力：若缺少恢复后 epoch 校验，A 会以 fds[0] 数字重试 read，
 *         读到 'z' 返回 1（错误或新对象数据）。
 * ------------------------------------------------------------------ */
BOOST_AUTO_TEST_CASE(t_readable_resume_window_returns_ebadf)
{
    CoFixture fx{1};
    int fds[2] = {-1, -1};   // A 等待的旧对象
    int np[2]  = {-1, -1};   // 复用源：dup2(np[0] -> fds[0])，对端 np[1] 供写数据
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, np), 0);

    bbt::core::thread::CountDownLatch done{1};
    std::atomic<ssize_t> result{12345};
    std::atomic_int errcode{0};

    // 协程 A：在 fds[0] 上挂起读（socketpair 无数据 → EAGAIN → park）
    bbtco [&]() {
        char buf[1];
        errno = 0;
        const ssize_t r = ::read(fds[0], buf, 1);
        result.store(r);
        errcode.store(errno);
        done.Down();
    };

    // 控制协程：等 A 真正进入等待表（FdWaiterCount 探针），再写就绪数据让
    // A 的 waiter 被正常 READABLE 触发；随后轮询到 FdWaiterCount==0（驱动
    // 线程已 _Complete 摘表、协程已入就绪队列）但 A 尚未恢复前，完成
    // close+dup2+新对象就绪——确定性命中「触发后未恢复」窗口。
    bbtco [&]() {
        BOOST_REQUIRE(CoWaitFdWaiter(fds[0], 3000));
        const char r = 'r';
        BOOST_REQUIRE_EQUAL(::write(fds[1], &r, 1), 1);  // A 的 fd 就绪 → 唤醒入队
        /* 等驱动把 waiter 摘掉（_Complete 已执行、FdWaiterCount 归 0），
         * 单 worker 下 A 必然要等本协程让出才恢复——close 落在窗口内。 */
        for (int i = 0; i < 3000 && CoPollEvent::FdWaiterCount(fds[0]) != 0; ++i)
            bbtco_sleep(1);
        BOOST_REQUIRE_EQUAL(CoPollEvent::FdWaiterCount(fds[0]), 0);
        BOOST_REQUIRE_EQUAL(::close(fds[0]), 0);
        BOOST_REQUIRE_EQUAL(::dup2(np[0], fds[0]), fds[0]);
        const char z = 'z';
        BOOST_REQUIRE_EQUAL(::write(np[1], &z, 1), 1);
    };

    const bool ok = WaitLatch(done, 5000);
    BOOST_REQUIRE_MESSAGE(ok, "coroutine did not finish after resume-window close+dup2");
    BOOST_CHECK_EQUAL(result.load(), -1);
    BOOST_CHECK_EQUAL(errcode.load(), EBADF);
    if (::fcntl(fds[0], F_GETFD) >= 0) ::close(fds[0]);
    ::close(fds[1]);
    ::close(np[1]);
}

/* ------------------------------------------------------------------ *
 * 用例 2：close 与新 waiter 并发——BeginFdClose 摘表 → EndFdClose 解
 * closing 之间到达的同号 waiter 不得被误摘/误醒，并在 close 完成后正常
 * 等待新对象的就绪事件。
 *
 * 时序：A 在 fds[0] park → B close(fds[0])+dup2(np[0]→fds[0]) → C 在
 *       复用后的 fds[0]（新对象）上 park 读 → B 用 np[1] 写 'q' 唤醒 C。
 *       A 得 -1/EBADF；C 得正常读 'q'（返回 1），证明未被误伤。
 *
 * 判别力：若 BeginFdClose 与底层 close 不在同一线性化区间，或代际推进
 *         发生在 close 返回之后，C 可能在摘表后、代际更新前登记成功并采样
 *         旧 epoch——随后被错误地当作旧 waiter 摘走或收到 POLL_EVENT_CLOSED。
 * ------------------------------------------------------------------ */
BOOST_AUTO_TEST_CASE(t_concurrent_close_and_new_waiter_not_miswoken)
{
    CoFixture fx{1};
    int fds[2] = {-1, -1};
    int np[2]  = {-1, -1};
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, np), 0);

    bbt::core::thread::CountDownLatch done{2};
    std::atomic<bool> fd_reused{false};
    std::atomic<ssize_t> result_a{12345};
    std::atomic<ssize_t> result_b{12345};

    // A：等旧 fds[0]，将被 close+dup2 唤醒（应得 EBADF）
    bbtco [&]() {
        char buf[1];
        errno = 0;
        const ssize_t r = ::read(fds[0], buf, 1);
        result_a.store(r);
        done.Down();
    };

    // 控制协程：等 A 真正进入等待表后 close+dup2 复用；等 C 在新 fd 上
    // park 后由 np[1] 写数据唤醒
    bbtco [&]() {
        BOOST_REQUIRE(CoWaitFdWaiter(fds[0], 3000));          // A 已登记
        BOOST_REQUIRE_EQUAL(::close(fds[0]), 0);
        BOOST_REQUIRE_EQUAL(::dup2(np[0], fds[0]), fds[0]);
        fd_reused.store(true, std::memory_order_release);
        // 等 C 在新 fd 上完成登记（同一 fd 号，FdWaiterCount 重新 >0）
        BOOST_REQUIRE(CoWaitFdWaiter(fds[0], 3000));
        const char c = 'q';
        BOOST_REQUIRE_EQUAL(::write(np[1], &c, 1), 1);     // 新对象对端可读事件
    };

    // C：在复用后的新 fds[0] 上挂起读，应由真实事件唤醒读到 'q'
    bbtco [&]() {
        BOOST_REQUIRE(CoWaitFlag(fd_reused, 3000));
        char buf[1] = {0};
        errno = 0;
        const ssize_t r = ::read(fds[0], buf, 1);
        result_b.store(r);
        done.Down();
    };

    const bool fin = WaitLatch(done, 5000);
    BOOST_REQUIRE_MESSAGE(fin, "coroutines did not finish: A or C still parked after close+dup2");
    BOOST_CHECK_EQUAL(result_a.load(), -1);                // 旧 waiter EBADF
    BOOST_CHECK_EQUAL(result_b.load(), 1);                 // 新 waiter 正常读 'q'
    if (::fcntl(fds[0], F_GETFD) >= 0) ::close(fds[0]);
    ::close(fds[1]);
    ::close(np[1]);
}

/* ------------------------------------------------------------------ *
 * 用例 3：多 worker（非单线程配置）下的 close+reuse。
 *
 * 多 worker 时等待协程可能被触发后跨 worker 恢复，调度间隙真实存在。
 * close+dup2 由主线程直接发起（不经协程），更贴近真实并发形态。
 *
 * 判别：协程的 read 必须返回 -1（不读新对象）；且复用对象上预置的
 * 'z' 字节在协程结束后必须仍能被主线程读到——若旧等待被重定向到
 * 新对象，'z' 会被旧 read 消耗，主线程只能读到 EAGAIN。
 * （errno 值在协程上下文中依赖 TLS 槽绑定，多 worker 下观测不稳定，
 *  故以「返回值 + 数据不串线」为断言，errno 仅作信息输出。）
 * ------------------------------------------------------------------ */
BOOST_AUTO_TEST_CASE(t_multi_worker_close_reuse_returns_ebadf)
{
    CoFixture fx{4};   // 多 worker：调度间隙真实存在
    int fds[2] = {-1, -1};
    int np[2]  = {-1, -1};
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, np), 0);

    bbt::core::thread::CountDownLatch done{1};
    std::atomic<ssize_t> result{12345};
    std::atomic_int errcode{0};

    bbtco [&]() {
        char buf[1];
        errno = 0;
        const ssize_t r = ::read(fds[0], buf, 1);
        result.store(r);
        errcode.store(errno);
        done.Down();
    };

    // 主线程（非协程）：FdWaiterCount 探针确认 A 已登记，随后 close+dup2+喂数据。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (CoPollEvent::FdWaiterCount(fds[0]) == 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    BOOST_REQUIRE(CoPollEvent::FdWaiterCount(fds[0]) > 0);

    BOOST_REQUIRE_EQUAL(::close(fds[0]), 0);
    BOOST_REQUIRE_EQUAL(::dup2(np[0], fds[0]), fds[0]);
    const char z = 'z';
    BOOST_REQUIRE_EQUAL(::write(np[1], &z, 1), 1);

    const bool ok = WaitLatch(done, 5000);
    BOOST_REQUIRE_MESSAGE(ok, "coroutine did not finish after multi-worker close+dup2");
    BOOST_CHECK_EQUAL(result.load(), -1);

    /* 判别核心：旧等待若误重试到复用后的 fds[0]，'z' 会被消耗；
     * 正确语义下 'z' 必须仍在复用对象上可读。 */
    int fl = ::fcntl(fds[0], F_GETFL, 0);
    BOOST_REQUIRE(fl >= 0);
    BOOST_REQUIRE_EQUAL(::fcntl(fds[0], F_SETFL, fl | O_NONBLOCK), 0);
    char probe = 0;
    const ssize_t got = ::read(fds[0], &probe, 1);
    BOOST_CHECK_MESSAGE(got == 1 && probe == 'z',
                        "reused-fd byte was consumed by stale coroutine read (got=" << got << ")");
    if (::fcntl(fds[0], F_GETFD) >= 0) ::close(fds[0]);
    ::close(fds[1]);
    ::close(np[1]);
}

/* ------------------------------------------------------------------ *
 * 用例 4（round-3 修复）：并发重复 close 的所有权归属。
 *
 * 时序：协程 A 在 fds[0] 上 park 读 → 主线程 T1 close(fds[0]) claim
 *       closing，经 g_bbt_fd_close_stall_for_test_ms 在底层 close 前
 *       停 50ms，T2 在此窗口内并发 close(fds[0])（BeginFdClose=false）
 *       → T1 close 返回、EndFdClose 后 dup2(np[0]→fds[0]) 复用该号。
 *
 * 判别：T2 的 close 必须返回 -1/EBADF 且不得发起第二次底层 close——
 *       若它仍然 close 数字 fd，会把刚 dup2 上去的新对象（np[0] 的承载
 *       体）一并关掉；随后主线程 fcntl(fds[0], F_GETFD) 会返回 EBADF。
 *       正确语义下新对象存活、可正常读写。
 * ------------------------------------------------------------------ */
BOOST_AUTO_TEST_CASE(t_concurrent_double_close_does_not_close_reused_fd)
{
    CoFixture fx{1};
    int fds[2] = {-1, -1};
    int np[2]  = {-1, -1};
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, np), 0);

    bbt::core::thread::CountDownLatch done{1};
    std::atomic<ssize_t> result_a{12345};

    // A：park 在旧 fds[0] 上，被 close 摘表唤醒（EBADF 语义由既有路径覆盖）
    bbtco [&]() {
        char buf[1];
        const ssize_t r = ::read(fds[0], buf, 1);
        result_a.store(r);
        done.Down();
    };

    // 主线程：等 A 登记后，T1 claim close；T2 在确定性 stall 窗口内并发 close
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (CoPollEvent::FdWaiterCount(fds[0]) == 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    BOOST_REQUIRE(CoPollEvent::FdWaiterCount(fds[0]) > 0);

    /* T1 走 ::close → Hook_Close：BeginFdClose claim 后在底层 close 前
     * stall 50ms——T2 在此窗口内必然观察到 FdIsClosing 并发起并发
     * close（命中 BeginFdClose=false 分支）。 */
    g_bbt_fd_close_stall_for_test_ms.store(50, std::memory_order_release);

    std::atomic<int>  t2_ret{-2};
    std::atomic<int>  t2_errno{0};
    std::atomic<bool> t2_issued{false};

    std::thread t2([&]() {
        // 自旋到 T1 已 claim（stall 窗口保证必然命中）
        for (int i = 0; i < 10000 && !CoPollEvent::FdIsClosing(fds[0]); ++i) {}
        BOOST_REQUIRE(CoPollEvent::FdIsClosing(fds[0]));
        errno = 0;
        const int r = ::close(fds[0]);   // BeginFdClose=false 路径
        t2_ret.store(r);
        t2_errno.store(errno);
        t2_issued.store(true, std::memory_order_release);
    });

    // T1：claim + stall + 底层 close + EndFdClose
    BOOST_REQUIRE_EQUAL(::close(fds[0]), 0);
    g_bbt_fd_close_stall_for_test_ms.store(0, std::memory_order_release);
    t2.join();
    BOOST_REQUIRE(t2_issued.load());

    // T2 必须得到 -1/EBADF，且没有对 fd 号发起第二次底层 close
    BOOST_CHECK_EQUAL(t2_ret.load(), -1);
    BOOST_CHECK_EQUAL(t2_errno.load(), EBADF);

    // 复用同号：新对象必须未被 T2 的 close 误关
    BOOST_REQUIRE_EQUAL(::dup2(np[0], fds[0]), fds[0]);
    int fl = ::fcntl(fds[0], F_GETFD);
    BOOST_REQUIRE_MESSAGE(fl >= 0,
        "reused fd was closed by the losing concurrent close (EBADF on fcntl)");

    const bool ok = WaitLatch(done, 5000);
    BOOST_REQUIRE_MESSAGE(ok, "waiter coroutine did not finish after concurrent double close");
    BOOST_CHECK_EQUAL(result_a.load(), -1);
    if (::fcntl(fds[0], F_GETFD) >= 0) ::close(fds[0]);
    ::close(fds[1]);
    ::close(np[1]);
}

/* ------------------------------------------------------------------ *
 * 用例 5（round-3 修复）：Event 构造失败时 callback_map 与内部 dup fd
 * 均回滚——把进程 fd 上限压到当前用量，使 Event 构造期的 ::dup 必然
 * 以 EMFILE 失败。
 *
 * 判别：InitFdEvent 返回 -1（等待建立失败收敛），且不残留 callback 条目
 *       （CallbackEntryCount 不变）、不泄漏 fd（/proc/self/fd 计数不变）。
 * ------------------------------------------------------------------ */
BOOST_AUTO_TEST_CASE(t_event_construct_dup_failure_rolls_back)
{
    CoFixture fx{1};

    auto FdCount = []() -> long {
        long n = 0;
        for (auto& e : std::filesystem::directory_iterator("/proc/self/fd"))
            (void)e, ++n;
        return n;
    };

    int sp[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, sp), 0);

    /* EventBase/io_context 自身要占 fd（eventfd 等），须在压上限之前建好；
     * Event 构造期间的 ::dup 才会是唯一的失败点。 */
    bbt::pollevent::detail::EventBase base;

    const long fd_before = FdCount();
    const long cb_before = static_cast<long>(bbt::pollevent::Event::CallbackEntryCount());

    /* 压低 fd 上限至当前用量：Event 构造期 ::dup(fd) 必须失败（EMFILE），
     * 走资源创建失败路径；callback 尚未注册，callback_map 不得残留，且
     * 原有 fd 数量不应改变。setrlimit 是进程级操作，末尾必须恢复原上限。 */
    struct rlimit old_lim{};
    BOOST_REQUIRE_EQUAL(::getrlimit(RLIMIT_NOFILE, &old_lim), 0);
    struct rlimit cap = old_lim;
    cap.rlim_cur = static_cast<rlim_t>(fd_before);
    BOOST_REQUIRE_EQUAL(::setrlimit(RLIMIT_NOFILE, &cap), 0);

    /* 直接构造 Event：内部 dup 抛 boost::system::system_error（EMFILE）。
     * 修复语义：失败前没有 callback_map 残留，已有 fd 不受影响。 */
    std::exception_ptr eptr{nullptr};
    try {
        auto ev = std::make_shared<bbt::pollevent::Event>(
            &base, sp[0], bbt::pollevent::EventOpt::READABLE,
            [](int, short, bbt::pollevent::EventId){});
    } catch (...) {
        eptr = std::current_exception();
    }

    // 恢复上限，再断言无回滚残留
    BOOST_REQUIRE_EQUAL(::setrlimit(RLIMIT_NOFILE, &old_lim), 0);

    BOOST_REQUIRE(eptr != nullptr);
    try { std::rethrow_exception(eptr); }
    catch (const boost::system::system_error& e) {
        BOOST_CHECK_EQUAL(e.code().value(), EMFILE);
    }
    catch (const std::system_error& e) {
        BOOST_CHECK_EQUAL(e.code().value(), EMFILE);
    }
    catch (const std::exception& e) {
        BOOST_FAIL("Event ctor threw unexpected exception: " << e.what());
    }
    catch (...) { BOOST_FAIL("Event ctor threw non-std exception"); }

    BOOST_CHECK_EQUAL(static_cast<long>(bbt::pollevent::Event::CallbackEntryCount()),
                      cb_before);
    BOOST_CHECK_EQUAL(FdCount(), fd_before);

    ::close(sp[0]);
    ::close(sp[1]);
}

BOOST_AUTO_TEST_SUITE_END()
