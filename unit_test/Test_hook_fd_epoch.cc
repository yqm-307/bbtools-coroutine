#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <mutex>

// #370 FD epoch 线性化回归：close 后同号 fd 复用不得把旧等待/新等待串线。
//
// 三类判别性用例：
//  1) 正常事件后恢复窗口：协程因 READABLE 唤醒但尚未恢复执行时，
//     另一线程 close+dup2 复用同号——恢复路径不得以旧 fd 数字重试
//     syscall（必须返回 -1/EBADF，不得读到新对象数据）。
//  2) close 与新 waiter 并发：BeginFdClose 摘表后到 EndFdClose 解 closing
//     之间的窗口内，同号 fd 上的新 waiter 不得被误摘/误醒；底层 close
//     完成后新 waiter 采样新一代际，应被真实事件正常唤醒。
//  （多 worker close+reuse 移到独立可执行 Test_hook_fd_epoch_multi，见该文件。）
//
// 同步约定：本文件全部用单 worker（m_cfg_static_thread_num=1）；
// 协程内禁止 CountDownLatch::Wait（阻塞 worker 会饿死同 worker 其它协程），
// 一律用 atomic flag + bbtco_sleep 让出轮询；主线程侧用 WaitLatch 有界等待。
// 复用源一律取独立 socketpair（dup2 同 socket 会因对端已关闭而 EPIPE）。

#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/pollevent/Event.hpp>
#include <bbt/pollevent/EventLoop.hpp>

/* 进程寿命模型：runtime 只初始化一次，重复 Start 抛 std::logic_error。 */
namespace
{
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, [](){
        bbt::coroutine::detail::Scheduler::GetInstance()->Start();
    });
}
}

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
        EnsureRuntime();
    }
    ~CoFixture()
    {
        std::signal(SIGPIPE, m_old_pipe);
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

/* 用例 3（多 worker close+reuse）已移至独立可执行 Test_hook_fd_epoch_multi：
 * worker 数在首次 Start 时固定，同进程内无法从 1 切换为 4（一次初始化契约）。 */

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
 * 用例 5（round-3 修复 / r2 稳定度量）：Event 构造失败时 callback_map 与
 * 内部 dup fd 均回滚——把进程 fd 上限压到当前占用，使 Event 构造期的
 * ::dup 必然以 EMFILE 失败。
 *
 * 度量口径（r1 已证原断言不可用）：原实现只比较 /proc/self/fd 里的**最大
 * fd 号**，而迭代目录本身会占用一个 fd 且被计入结果，fd 分配器又总是复用
 * 最小空闲号——最大值会随无关的分配顺序上下浮动（实测失败方向是「变少」
 * 12 != 13），既不证明泄漏也不证明回滚。这里改成：排除测量自身 fd 后的
 * **fd 集合**比较，对「多出」和「少掉」两个方向都敏感，且与 fd 号复用无关。
 * RLIMIT_NOFILE 是进程级限制，用 RAII 恢复，避免断言提前终止后污染后续用例。
 *
 * 判别：InitFdEvent / Event 构造失败必须收敛为 EMFILE 异常，且不残留
 *       callback 条目（CallbackEntryCount 不变）、fd 集合一个不多一个不少。
 * ------------------------------------------------------------------ */
struct RlimitRestoreGuard
{
    explicit RlimitRestoreGuard(const struct rlimit& saved) : m_saved(saved) {}
    ~RlimitRestoreGuard() { ::setrlimit(RLIMIT_NOFILE, &m_saved); }
    RlimitRestoreGuard(const RlimitRestoreGuard&) = delete;
    RlimitRestoreGuard& operator=(const RlimitRestoreGuard&) = delete;
    struct rlimit m_saved;
};

struct FillerFdGuard
{
    ~FillerFdGuard() { CloseAll(); }
    std::vector<int>& Fds() { return m_fds; }
    void CloseAll()
    {
        for (const int fd : m_fds)
            ::close(fd);
        m_fds.clear();
    }
    std::vector<int> m_fds;
};

/* 稳定 fd 快照：/proc/self/fd 集合剔除测量自身打开的目录 fd，升序返回。
 * 打开目录失败时返回空集（调用方以非空断言暴露）。 */
std::vector<int> OpenFdSnapshot()
{
    std::vector<int> fds;
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr)
        return fds;
    const int self_fd = ::dirfd(dir);
    while (struct dirent* ent = ::readdir(dir)) {
        char* end = nullptr;
        const long value = std::strtol(ent->d_name, &end, 10);
        if (end == ent->d_name || *end != '\0')
            continue;                       // ".", ".." 等非数字项
        if (static_cast<int>(value) == self_fd)
            continue;                       // 测量自身，不计入
        fds.push_back(static_cast<int>(value));
    }
    ::closedir(dir);
    std::sort(fds.begin(), fds.end());
    return fds;
}

BOOST_AUTO_TEST_CASE(t_event_construct_dup_failure_rolls_back)
{
    CoFixture fx{1};

    int sp[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, sp), 0);

    /* EventBase/io_context 自身要占 fd（eventfd 等），须在压上限之前建好；
     * Event 构造期间的 ::dup 才会是唯一的失败点。 */
    bbt::pollevent::detail::EventBase base;

    const std::vector<int> fds_before = OpenFdSnapshot();
    /* 度量稳定性自证：同状态下连续两次快照必须一致（排除测量自身 fd 后
     * 与 fd 号复用、分配顺序无关）。 */
    BOOST_REQUIRE(fds_before == OpenFdSnapshot());
    BOOST_REQUIRE(!fds_before.empty());
    const long cb_before = static_cast<long>(bbt::pollevent::Event::CallbackEntryCount());

    /* RLIMIT_NOFILE 限制的是 fd 数值上限而非当前 fd 数量。先压到当前最大
     * fd+1，再用 openat 填满上限以下的空洞，确保 Event 构造期 ::dup(fd)
     * 稳定失败（EMFILE），走资源创建失败路径；callback 尚未注册，
     * callback_map 不得残留。setrlimit 是进程级操作，由 RAII 保证恢复。 */
    struct rlimit old_lim{};
    BOOST_REQUIRE_EQUAL(::getrlimit(RLIMIT_NOFILE, &old_lim), 0);
    struct rlimit cap = old_lim;
    cap.rlim_cur = static_cast<rlim_t>(fds_before.back() + 1);
    BOOST_REQUIRE_EQUAL(::setrlimit(RLIMIT_NOFILE, &cap), 0);
    RlimitRestoreGuard restore_limit{old_lim};

    FillerFdGuard fillers;
    for (;;) {
        const int filler = static_cast<int>(::syscall(
            SYS_openat, AT_FDCWD, "/dev/null", O_RDONLY | O_CLOEXEC, 0));
        if (filler < 0) {
            BOOST_REQUIRE_EQUAL(errno, EMFILE);
            break;
        }
        fillers.Fds().push_back(filler);
    }

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

    /* 恢复上限与 filler 后再断言：空洞填满状态下无法再打开目录 fd 做快照。 */
    BOOST_REQUIRE_EQUAL(::setrlimit(RLIMIT_NOFILE, &old_lim), 0);
    fillers.CloseAll();

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

    const std::vector<int> fds_after = OpenFdSnapshot();
    BOOST_CHECK_MESSAGE(fds_after.size() == fds_before.size(),
        "fd count changed across failed Event ctor: before=" << fds_before.size()
        << " after=" << fds_after.size());
    BOOST_CHECK_MESSAGE(fds_after == fds_before,
        "fd set changed across failed Event ctor (rollback leak or spurious close)");
    BOOST_CHECK_EQUAL(static_cast<long>(::fcntl(sp[0], F_GETFD)), 0);  // 原 fd 未被误关
    ::close(sp[0]);
    ::close(sp[1]);
}

BOOST_AUTO_TEST_SUITE_END()
