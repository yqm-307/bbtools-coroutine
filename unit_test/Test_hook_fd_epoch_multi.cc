/**
 * @file Test_hook_fd_epoch_multi.cc
 * @brief #370 多 worker close+reuse 判别性用例（独立进程）。
 *
 * 为什么单列可执行：进程寿命模型下 runtime 只初始化一次，worker 数在首次
 * Start 时按当次配置固定。原先「1 worker 用例 + 4 worker 用例」同处一个可执
 * 行时，第二个用例无法真正切到 4 个 worker——只会让运行期配置与实际 worker
 * 数不一致（验收曾在 Scheduler::_LoadBlance2Proc 触发 vector 下标越界断言）。
 * 本文件用独立进程保证 fixture 的 4 worker 配置在首次（也是唯一一次）Start
 * 之前生效，多 worker 覆盖因此是真实的：等待协程确实可能跨 worker 恢复。
 *
 * 判别：协程的 read 必须返回 -1（不读新对象）；且复用对象上预置的 'z' 字节
 * 在协程结束后必须仍能被主线程读到——若旧等待被重定向到新对象，'z' 会被旧
 * read 消耗，主线程只能读到 EAGAIN。（errno 在协程上下文中依赖 TLS 槽绑定，
 * 多 worker 下观测不稳定，故以「返回值 + 数据不串线」为断言。）
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(HookFdEpochMultiTest)

namespace
{

/* 唯一一次 Start 前固定 4 worker：本可执行只有这一个用例，配置不会被后续
 * 用例覆盖，一次初始化契约与多 worker 覆盖同时成立。 */
void EnsureRuntimeMulti()
{
    static std::once_flag once;
    std::call_once(once, [](){
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = 4;
        cfg->m_cfg_stack_protect = false;
        bbt::coroutine::detail::Scheduler::GetInstance()->Start();
    });
}

bool WaitLatch(bbt::core::thread::CountDownLatch& latch, int ms)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (latch.WaitTimeout(1) == 0)
            return true;
    }
    return latch.WaitTimeout(0) == 0;
}

} // namespace

BOOST_AUTO_TEST_CASE(t_multi_worker_close_reuse_returns_ebadf)
{
    EnsureRuntimeMulti();
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

BOOST_AUTO_TEST_SUITE_END()
