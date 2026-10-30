/**
 * @file Test_event_constructor_rollback.cc
 * @brief Event 构造在 timer 创建点失败时，内部 dup fd 只被关闭一次。
 *
 * 旧实现先 ::close(m_internal_fd)，再 m_sd.reset() 触发 stream_descriptor
 * 析构对同一 fd 号二次 close；两次 close 之间若有他线程 open/dup 拿到同号，
 * 新对象会被误关。本测试用 ELF 符号插入做确定性判别：
 *
 *   - operator new(size)：仅当 size == sizeof(steady_timer) 且已武装时抛
 *     bad_alloc——把失败精确钉在 make_unique<steady_timer> 的分配点，
 *     不依赖中间分配次数（不新增生产 seam/公共 API）。
 *   - close(fd)：本进程内对「预测的内部 fd 号」的每次 close 计数；第一次
 *     close 之后立刻用 dup 把同号 fd 重新占住（模拟他线程复用），第二次
 *     close 就会误杀该 victim——修复后 victim 必须存活且关闭计数恰为 1。
 *
 * 进程模型：不启动 Scheduler，直接构造 detail::EventBase + Event；
 * Event 只依赖 EventBase::GetContext()/IncEventCount。两模式各断言互补：
 *   rollback_timer_fail —— 注入失败路径：关一次、victim 存活、无孤儿 callback；
 *   construct_ok        —— 未注入对照：构造成功、fd+timer 都在、正常析构不残留。
 */

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <sys/syscall.h>
#include <unistd.h>

#include <boost/asio/steady_timer.hpp>
#include <bbt/pollevent/Event.hpp>
#include <bbt/pollevent/detail/EventBase.hpp>

/* EventBase 需要非空；放函数内避免静态析构顺序问题 */
static bbt::pollevent::detail::EventBase* nullptr_safe_base();

namespace
{

/* ── operator new 注入：只对 sizeof(steady_timer) 的分配生效 ── */
constexpr std::size_t kTimerSize = sizeof(boost::asio::steady_timer);
std::atomic_bool      g_fail_timer_alloc{false};
std::atomic_int       g_new_hits{0};

/* ── close 观察：对 watched fd 计数；首次 close 后立即重占同号 ── */
std::atomic_int       g_watched_fd{-1};
std::atomic_int       g_watched_closes{0};
std::atomic_int       g_victim_fd{-1};   /* 复用同号的存活对象 */
int                   g_anchor_fd{-1};   /* dup 源，常开 */

void ReoccupyWatchedFd()
{
    /* 刚被 close 的 fd 号是进程中当前最低空闲号：dup 立刻把它占回，
     * 等价于「两次 close 之间他线程 open 拿到同号」的确定性形态。 */
    int victim = ::dup(g_anchor_fd);
    if (victim == g_watched_fd.load(std::memory_order_acquire))
        g_victim_fd.store(victim, std::memory_order_release);
    else if (victim >= 0)
        ::syscall(SYS_close, victim);   /* 未占到同号：不留垃圾 */
}

} // namespace

void* operator new(std::size_t n)
{
    if (n == kTimerSize &&
        g_fail_timer_alloc.load(std::memory_order_acquire))
    {
        g_fail_timer_alloc.store(false, std::memory_order_release);
        g_new_hits.fetch_add(1, std::memory_order_acq_rel);
        throw std::bad_alloc();
    }
    if (auto* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

/* close 插入：可执行自身定义优先于共享库内 PLT 解析（--export-dynamic）。
 * 真实 close 走 SYS_close 直递内核；不递归，不依赖 dlsym。 */
extern "C" int close(int fd)
{
    int rc = static_cast<int>(::syscall(SYS_close, fd));
    if (rc == 0 && fd == g_watched_fd.load(std::memory_order_acquire))
    {
        g_watched_closes.fetch_add(1, std::memory_order_acq_rel);
        ReoccupyWatchedFd();
    }
    return rc;
}

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "FAIL line=%d %s closes=%d victim=%d new_hits=%d\n", \
        __LINE__, #x, g_watched_closes.load(), g_victim_fd.load(), \
        g_new_hits.load()); return 1; } } while (0)

namespace
{

int FdCount()
{
    /* /proc/self/fd 计数（含本次打开的目录自身 fd，作差比较不受影响） */
    int n = 0;
    if (auto* d = ::opendir("/proc/self/fd"))
    {
        while (::readdir(d) != nullptr) ++n;
        ::closedir(d);
    }
    return n;
}

/* 预测 Event 构造内部 ::dup 会拿到的 fd 号：当前最低空闲号。 */
int PredictInternalFd()
{
    int probe = ::dup(g_anchor_fd);
    if (probe < 0) return -1;
    ::syscall(SYS_close, probe);
    return probe;   /* 此刻进程内无其他线程分配 fd，dup 必取同一最低号 */
}

int RunTimerFailRollback()
{
    namespace pe = bbt::pollevent;

    g_anchor_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(g_anchor_fd >= 0);

    /* EventBase 首次构造会开 epoll/eventfd：先实例化再预测内部 fd 号，
     * 否则这些基建 fd 会抢占最低空闲号使预测失准。
     * Boost 1.90 io_context 惰性初始化：首个 stream_descriptor 构造才开
     * reactor 基建 fd（epoll/eventfd/timerfd 共 ~3 个）。先用一个临时
     * Event 预热 reactor，fd_before 才只含本用例自有资源。 */
    auto* base = nullptr_safe_base();
    CHECK(base != nullptr);
    {
        int wf[2];
        CHECK(::pipe2(wf, O_NONBLOCK | O_CLOEXEC) == 0);
        pe::Event warmup(base, wf[0],
                         static_cast<short>(pe::EventOpt::READABLE),
                         [](int, short, pe::EventId) {});
        ::close(wf[0]);
        ::close(wf[1]);
    }

    int fds[2];
    CHECK(::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
    const int target = fds[0];

    const int fd_before = FdCount();
    const auto cb_before = pe::Event::CallbackEntryCount();

    /* 先完成所有会 open/close 的探测，再武装 watched：此后构造路径内
     * 对 watched 号的每次真实 close 都被计数，探测动作不再干扰。 */
    const int watched = PredictInternalFd();
    CHECK(watched > 0);
    g_watched_fd.store(watched, std::memory_order_release);

    g_fail_timer_alloc.store(true, std::memory_order_release);
    bool threw = false;
    try
    {
        /* fd 事件 + timer：走 is_fd_event && is_timer 双分支，
         * 内部 dup → stream_descriptor adopt → timer 构造点注入失败。 */
        pe::Event ev(base, target,
                     static_cast<short>(pe::EventOpt::READABLE | pe::EventOpt::TIMEOUT),
                     [](int, short, pe::EventId) {});
    }
    catch (const std::bad_alloc&)
    {
        threw = true;
    }
    catch (...)
    {
        threw = true;
    }
    g_watched_fd.store(-1, std::memory_order_release);   /* 观察窗结束 */

    CHECK(threw);                                /* 注入必须真实命中 */
    CHECK(g_new_hits.load() == 1);

    /* 核心断言：watched（内部 dup fd）只被关闭一次；首次 close 后
     * 复用同号的 victim 仍然存活——旧实现第二次 close 会把它误关。 */
    CHECK(g_watched_closes.load() == 1);
    const int victim = g_victim_fd.load(std::memory_order_acquire);
    CHECK(victim == watched);                    /* 同号复用确实发生 */
    errno = 0;
    CHECK(::fcntl(victim, F_GETFD) >= 0);        /* victim 未被误关 */

    /* 用户 fd 不受影响；callback_map 无孤儿条目；无 fd 泄漏：
     * 进程 fd 总数与构造前一致 + victim（占住 watched 号）+ 未动。 */
    errno = 0;
    CHECK(::fcntl(target, F_GETFD) >= 0);
    CHECK(pe::Event::CallbackEntryCount() == cb_before);
    CHECK(FdCount() == fd_before + 1);           /* +1 = victim 占住的 watched */

    ::syscall(SYS_close, victim);
    ::syscall(SYS_close, target);
    ::syscall(SYS_close, fds[1]);
    ::syscall(SYS_close, g_anchor_fd);
    g_watched_fd.store(-1, std::memory_order_release);
    return 0;
}

int RunConstructOk()
{
    namespace pe = bbt::pollevent;

    g_anchor_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(g_anchor_fd >= 0);

    auto* base = nullptr_safe_base();   /* 先建 EventBase + 预热 reactor 基建 fd */
    CHECK(base != nullptr);
    {
        int wf[2];
        CHECK(::pipe2(wf, O_NONBLOCK | O_CLOEXEC) == 0);
        pe::Event warmup(base, wf[0],
                         static_cast<short>(pe::EventOpt::READABLE),
                         [](int, short, pe::EventId) {});
        ::close(wf[0]);
        ::close(wf[1]);
    }

    int fds[2];
    CHECK(::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
    const int target = fds[0];

    const int fd_before = FdCount();
    const auto cb_before = pe::Event::CallbackEntryCount();

    const int watched = PredictInternalFd();
    CHECK(watched > 0);
    g_watched_fd.store(watched, std::memory_order_release);

    {
        pe::Event ev(base, target,
                     static_cast<short>(pe::EventOpt::READABLE | pe::EventOpt::TIMEOUT),
                     [](int, short, pe::EventId) {});
        /* 未注入：构造成功，内部 fd 已 adopt 但尚未 close */
        CHECK(g_watched_closes.load() == 0);
        CHECK(pe::Event::CallbackEntryCount() == cb_before + 1);
    }
    /* 正常析构（CancelListen）：内部 fd 恰好关闭一次 */
    CHECK(g_watched_closes.load() == 1);
    g_watched_fd.store(-1, std::memory_order_release);   /* 观察窗结束 */
    /* 析构路径的 ReoccupyWatchedFd 也可能占号：若占住则收回后核对平衡 */
    const int victim = g_victim_fd.load(std::memory_order_acquire);
    if (victim == watched)
        ::syscall(SYS_close, victim);

    CHECK(pe::Event::CallbackEntryCount() == cb_before);
    CHECK(FdCount() == fd_before);
    errno = 0;
    CHECK(::fcntl(target, F_GETFD) >= 0);        /* 用户 fd 由调用方拥有 */

    ::syscall(SYS_close, target);
    ::syscall(SYS_close, fds[1]);
    ::syscall(SYS_close, g_anchor_fd);
    return 0;
}

} // namespace

/* EventBase 需要非空；放函数内避免静态析构顺序问题 */
static bbt::pollevent::detail::EventBase* nullptr_safe_base()
{
    static bbt::pollevent::detail::EventBase base;
    return &base;
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    int rc = 1;
    if (!std::strcmp(argv[1], "rollback_timer_fail"))
        rc = RunTimerFailRollback();
    else if (!std::strcmp(argv[1], "construct_ok"))
        rc = RunConstructOk();
    else
        std::fprintf(stderr, "FAIL unknown mode=%s\n", argv[1]);

    if (rc == 0)
        std::fprintf(stderr, "PASS mode=%s closes=%d victim=%d new_hits=%d\n",
                     argv[1], g_watched_closes.load(), g_victim_fd.load(),
                     g_new_hits.load());
    return rc;
}
